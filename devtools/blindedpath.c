#include "config.h"
#include <assert.h>
#include <bitcoin/privkey.h>
#include <ccan/err/err.h>
#include <ccan/mem/mem.h>
#include <ccan/opt/opt.h>
#include <ccan/str/hex/hex.h>
#include <ccan/tal/tal.h>
#include <common/hmac.h>
#include <common/sphinx.h>
#include <common/type_to_string.h>
#include <common/utils.h>
#include <common/version.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <sodium/crypto_auth_hmacsha256.h>
#include <sodium/crypto_aead_chacha20poly1305.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Tal wrappers for opt. */
static void *opt_allocfn(size_t size)
{
	return tal_arr_label(NULL, char, size, TAL_LABEL("opt_allocfn", ""));
}

static void *tal_reallocfn(void *ptr, size_t size)
{
	if (!ptr)
		return opt_allocfn(size);
	tal_resize_(&ptr, 1, size, false);
	return ptr;
}

static void tal_freefn(void *ptr)
{
	tal_free(ptr);
}

/* E(i-1) = H(E(i) || ss(i)) * E(i) */
static struct sha256 hash_e_and_ss(const struct pubkey *e,
				   const struct secret *ss)
{
	u8 der[PUBKEY_CMPR_LEN];
	struct sha256_ctx shactx;
	struct sha256 h;

	pubkey_to_der(der, e);
	sha256_init(&shactx);
	sha256_update(&shactx, der, sizeof(der));
	sha256_update(&shactx, ss->data, sizeof(ss->data));
	sha256_done(&shactx, &h);

	return h;
}

/* E(i-1) = H(E(i) || ss(i)) * E(i) */
static struct pubkey next_pubkey(const struct pubkey *pk,
				 const struct sha256 *h)
{
	struct pubkey ret;

	ret = *pk;
	if (secp256k1_ec_pubkey_tweak_mul(secp256k1_ctx, &ret.pubkey, h->u.u8)
	    != 1)
		abort();

	return ret;
}

/* e(i+1) = H(E(i) || ss(i)) * e(i) */
static struct privkey next_privkey(const struct privkey *e,
				   const struct sha256 *h)
{
	struct privkey ret;

	ret = *e;
	if (secp256k1_ec_privkey_tweak_mul(secp256k1_ctx, ret.secret.data,
					   h->u.u8) != 1)
		abort();

	return ret;
}

int main(int argc, char **argv)
{
	bool first = false;

	setup_locale();

	secp256k1_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY |
						 SECP256K1_CONTEXT_SIGN);

	opt_set_alloc(opt_allocfn, tal_reallocfn, tal_freefn);
	opt_register_noarg("--help|-h", opt_usage_and_exit,
			   "\n\n\tcreate <nodeid>[/<scid>]...\n"
			   "\tunwrap <privkey> <onion> <blinding>\n",
			   "Show this message");
	opt_register_noarg("--first-node", opt_set_bool, &first,
			   "Don't try to tweak key to unwrap onion");
	opt_register_version();

	opt_parse(&argc, argv, opt_log_stderr_exit);
	setup_tmpctx();

	if (argc < 2)
		errx(1, "You must specify create or unwrap");
	if (streq(argv[1], "create")) {
		struct privkey e;
		struct pubkey *pk_e, *b, *nodes;
		struct secret *rho;
		size_t num = argc - 2;

		if (argc < 3)
			errx(1, "create requires at least one nodeid");

		/* P(i) */
		nodes = tal_arr(tmpctx, struct pubkey, num);
		/* E(i) */
		pk_e = tal_arr(tmpctx, struct pubkey, num);
		/* B(i) */
		b = tal_arr(tmpctx, struct pubkey, num);
		/* rho(i) */
		rho = tal_arr(tmpctx, struct secret, num);

		/* Randomness, chosen with a fair dice roll! */
		memset(&e, 6, sizeof(e));
		if (!pubkey_from_privkey(&e, &pk_e[0]))
			abort();

		for (size_t i = 0; i < num; i++) {
			struct secret ss;
			struct secret hmac;
			struct sha256 h;

			if (!pubkey_from_hexstr(argv[2+i],
						strcspn(argv[2+i], "/"),
						&nodes[i]))
				errx(1, "%s not a valid pubkey", argv[2+i]);

			if (secp256k1_ecdh(secp256k1_ctx, ss.data,
					   &nodes[i].pubkey, e.secret.data, NULL, NULL) != 1)
				abort();

			subkey_from_hmac("blinded_node_id", &ss, &hmac);
			b[i] = nodes[i];
			if (i != 0) {
				if (secp256k1_ec_pubkey_tweak_mul(secp256k1_ctx,
					  &b[i].pubkey, hmac.data) != 1)
					abort();
			}
			subkey_from_hmac("rho", &ss, &rho[i]);
			h = hash_e_and_ss(&pk_e[i], &ss);
			if (i != num-1)
				pk_e[i+1] = next_pubkey(&pk_e[i], &h);
			e = next_privkey(&e, &h);
		}

		/* Print initial blinding factor */
		printf("Blinding: %s\n",
		       type_to_string(tmpctx, struct pubkey, &pk_e[0]));

		for (size_t i = 0; i < num - 1; i++) {
			u8 *p;
			u8 buf[BIGSIZE_MAX_LEN];
			const unsigned char npub[crypto_aead_chacha20poly1305_ietf_NPUBBYTES] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
			struct tlv_onionmsg_payload *inner, *outer;
			int ret;

			/* Inner is encrypted */
			inner = tlv_onionmsg_payload_new(tmpctx);
			/* FIXME: Use /scid for encblob if specified */
			inner->next_node_id = tal(inner, struct tlv_onionmsg_payload_next_node_id);
			inner->next_node_id->node_id = nodes[i+1];
			p = tal_arr(tmpctx, u8, 0);
			towire_encmsg_tlvs(&p, inner);

			outer = tlv_onionmsg_payload_new(tmpctx);
			outer->enctlv = tal(outer, struct tlv_onionmsg_payload_enctlv);
			outer->enctlv->enctlv = tal_arr(tmpctx, u8, tal_count(p)
				      + crypto_aead_chacha20poly1305_ietf_ABYTES);
			ret = crypto_aead_chacha20poly1305_ietf_encrypt(outer->enctlv->enctlv, NULL,
									p,
									tal_bytelen(p),
									NULL, 0,
									NULL, npub,
									rho[i].data);
			assert(ret == 0);

			p = tal_arr(tmpctx, u8, 0);
			towire_onionmsg_payload(&p, outer);
			ret = bigsize_put(buf, tal_bytelen(p));

			/* devtools/onion wants length explicitly prepended */
			printf("%s/%.*s%s ",
			       type_to_string(tmpctx, struct pubkey, &b[i]),
			       ret * 2,
			       tal_hexstr(tmpctx, buf, ret),
			       tal_hex(tmpctx, p));
		}
		/* No payload for last node */
		printf("%s/00\n",
		       type_to_string(tmpctx, struct pubkey, &b[num-1]));
	} else if (streq(argv[1], "unwrap")) {
		struct privkey privkey;
		struct pubkey blinding;
		u8 onion[TOTAL_PACKET_SIZE], *dec;
		struct onionpacket op;
		struct secret ss, onion_ss;
		struct secret hmac, rho;
		struct route_step *rs;
		const u8 *cursor;
		struct tlv_onionmsg_payload *outer;
		size_t max, len;
		struct pubkey res;
		struct sha256 h;
		int ret;
		const unsigned char npub[crypto_aead_chacha20poly1305_ietf_NPUBBYTES] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

		if (argc != 5)
			errx(1, "unwrap requires privkey, onion and blinding");

		if (!hex_decode(argv[2], strlen(argv[2]), &privkey,
				sizeof(privkey)))
			errx(1, "Invalid private key hex '%s'", argv[2]);

		if (!hex_decode(argv[3], strlen(argv[3]), onion,
				sizeof(onion)))
			errx(1, "Invalid onion %s", argv[3]);

		if (!pubkey_from_hexstr(argv[4], strlen(argv[4]), &blinding))
			errx(1, "Invalid blinding %s", argv[4]);

		if (parse_onionpacket(onion, sizeof(onion), &op) != 0)
			errx(1, "Unparsable onion");

		/*   ss(r) = H(k(r) * E(r)) */
		if (secp256k1_ecdh(secp256k1_ctx, ss.data, &blinding.pubkey,
				   privkey.secret.data, NULL, NULL) != 1)
			abort();

		subkey_from_hmac("rho", &ss, &rho);

		/* b(i) = HMAC256("blinded_node_id", ss(i)) * k(i) */
		subkey_from_hmac("blinded_node_id", &ss, &hmac);

		/* We instead tweak the *ephemeral* key from the onion
		 * and use our raw privkey: this models how lightningd
		 * will do it, since hsmd knows only how to ECDH with
		 * our real key */
		res = op.ephemeralkey;
		if (!first) {
			if (secp256k1_ec_pubkey_tweak_mul(secp256k1_ctx,
							  &res.pubkey,
							  hmac.data) != 1)
				abort();
		}

		if (secp256k1_ecdh(secp256k1_ctx, onion_ss.data,
				   &res.pubkey,
				   privkey.secret.data, NULL, NULL) != 1)
			abort();

		rs = process_onionpacket(tmpctx, &op, &onion_ss, NULL, 0, false);
		if (!rs)
			errx(1, "Could not process onionpacket");

		cursor = rs->raw_payload;
		max = tal_bytelen(cursor);
		len = fromwire_bigsize(&cursor, &max);

		/* Always true since we're non-legacy */
		assert(len == max);
		outer = tlv_onionmsg_payload_new(tmpctx);
		if (!fromwire_onionmsg_payload(&cursor, &max, outer))
			errx(1, "Invalid payload %s",
			     tal_hex(tmpctx, rs->raw_payload));

		if (rs->nextcase == ONION_END) {
			printf("TERMINAL\n");
			return 0;
		}

		/* Look for enctlv */
		if (!outer->enctlv)
			errx(1, "No enctlv field");

		if (tal_bytelen(outer->enctlv->enctlv)
		    < crypto_aead_chacha20poly1305_ietf_ABYTES)
			errx(1, "enctlv field too short");

		dec = tal_arr(tmpctx, u8,
			      tal_bytelen(outer->enctlv->enctlv)
			      - crypto_aead_chacha20poly1305_ietf_ABYTES);
		ret = crypto_aead_chacha20poly1305_ietf_decrypt(dec, NULL,
								NULL,
								outer->enctlv->enctlv,
								tal_bytelen(outer->enctlv->enctlv),
								NULL, 0,
								npub,
								rho.data);
		if (ret != 0)
			errx(1, "Failed to decrypt enctlv field");

		printf("Contents: %s\n", tal_hex(tmpctx, dec));

		/* E(i-1) = H(E(i) || ss(i)) * E(i) */
		h = hash_e_and_ss(&blinding, &ss);
		res = next_pubkey(&blinding, &h);
		printf("Next blinding: %s\n",
		       type_to_string(tmpctx, struct pubkey, &res));
		printf("Next onion: %s\n", tal_hex(tmpctx, serialize_onionpacket(tmpctx, rs->next)));
	} else
		errx(1, "Either create or unwrap!");
}
