#include <ccan/cast/cast.h>
#include <ccan/crypto/siphash24/siphash24.h>
#include <ccan/tal/str/str.h>
#include <ccan/tal/tal.h>
#include <common/htlc.h>
#include <common/memleak.h>
#include <common/pseudorand.h>
#include <lightningd/htlc_end.h>
#include <lightningd/log.h>
#include <stdio.h>

size_t hash_htlc_key(const struct htlc_key *k)
{
	struct siphash24_ctx ctx;
	siphash24_init(&ctx, siphash_seed());
	/* channel doesn't move while in this hash, so we just hash pointer. */
	siphash24_update(&ctx, &k->channel, sizeof(k->channel));
	siphash24_u64(&ctx, k->id);

	return siphash24_done(&ctx);
}

struct htlc_in *find_htlc_in(const struct htlc_in_map *map,
			       const struct channel *channel,
			       u64 htlc_id)
{
	const struct htlc_key key = { (struct channel *)channel, htlc_id };
	return htlc_in_map_get(map, &key);
}

static void destroy_htlc_in(struct htlc_in *hend, struct htlc_in_map *map)
{
	htlc_in_map_del(map, hend);
}

void connect_htlc_in(struct htlc_in_map *map, struct htlc_in *hend)
{
	tal_add_destructor2(hend, destroy_htlc_in, map);
	htlc_in_map_add(map, hend);
}

struct htlc_out *find_htlc_out(const struct htlc_out_map *map,
			       const struct channel *channel,
			       u64 htlc_id)
{
	const struct htlc_key key = { (struct channel *)channel, htlc_id };
	return htlc_out_map_get(map, &key);
}

static void destroy_htlc_out(struct htlc_out *hend, struct htlc_out_map *map)
{
	htlc_out_map_del(map, hend);
}

void connect_htlc_out(struct htlc_out_map *map, struct htlc_out *hend)
{
	tal_add_destructor2(hend, destroy_htlc_out, map);
	htlc_out_map_add(map, hend);
}

static void *PRINTF_FMT(2,3)
	corrupt(const char *abortstr, const char *fmt, ...)
{
	if (abortstr) {
		char *p;
		va_list ap;

		va_start(ap, fmt);
		p = tal_vfmt(NULL, fmt, ap);
		fatal("%s:%s\n", abortstr, p);
		va_end(ap);
	}
	return NULL;
}

struct htlc_in *htlc_in_check(const struct htlc_in *hin, const char *abortstr)
{
	if (amount_msat_eq(hin->msat, AMOUNT_MSAT(0)))
		return corrupt(abortstr, "zero msatoshi");
	else if (htlc_state_owner(hin->hstate) != REMOTE)
		return corrupt(abortstr, "invalid state %s",
			       htlc_state_name(hin->hstate));
	else if (hin->failuremsg && hin->preimage)
		return corrupt(abortstr, "Both failuremsg and succeeded");
	else if (hin->failcode != 0 && hin->preimage)
		return corrupt(abortstr, "Both failcode and succeeded");
	else if (hin->failuremsg && (hin->failcode & BADONION))
		return corrupt(abortstr, "Both failed and malformed");

	/* Can't have a resolution while still being added. */
	if (hin->hstate >= RCVD_ADD_HTLC
	    && hin->hstate <= RCVD_ADD_ACK_REVOCATION) {
		if (hin->preimage)
			return corrupt(abortstr, "Still adding, has preimage");
		if (hin->failuremsg)
			return corrupt(abortstr, "Still adding, has failmsg");
		if (hin->failcode)
			return corrupt(abortstr, "Still adding, has failcode");
	} else if (hin->hstate >= SENT_REMOVE_HTLC
		   && hin->hstate <= SENT_REMOVE_ACK_REVOCATION) {
		if (!hin->preimage && !hin->failuremsg && !hin->failcode)
			return corrupt(abortstr, "Removing, no resolution");
	} else
		return corrupt(abortstr, "Bad state %s",
			       htlc_state_name(hin->hstate));

	return cast_const(struct htlc_in *, hin);
}

struct htlc_in *new_htlc_in(const tal_t *ctx,
			    struct channel *channel, u64 id,
			    struct amount_msat msat, u32 cltv_expiry,
			    u32 timestamp,
			    const struct sha256 *payment_hash,
			    const struct secret *shared_secret TAKES,
			    const u8 *onion_routing_packet)
{
	struct htlc_in *hin = tal(ctx, struct htlc_in);

	hin->dbid = 0;
	hin->key.channel = channel;
	hin->key.id = id;
	hin->msat = msat;
	hin->cltv_expiry = cltv_expiry;
	hin->timestamp = timestamp;
	hin->payment_hash = *payment_hash;
	if (shared_secret)
		hin->shared_secret = tal_dup(hin, struct secret, shared_secret);
	else
		hin->shared_secret = NULL;
	memcpy(hin->onion_routing_packet, onion_routing_packet,
	       sizeof(hin->onion_routing_packet));

	hin->hstate = RCVD_ADD_COMMIT;
	hin->failcode = 0;
	hin->failuremsg = NULL;
	hin->preimage = NULL;

	hin->received_time = time_now();

	return htlc_in_check(hin, "new_htlc_in");
}

struct htlc_out *htlc_out_check(const struct htlc_out *hout,
				const char *abortstr)
{
	if (htlc_state_owner(hout->hstate) != LOCAL)
		return corrupt(abortstr, "invalid state %s",
			       htlc_state_name(hout->hstate));
	else if (hout->failuremsg && hout->preimage)
		return corrupt(abortstr, "Both failed and succeeded");

	if (hout->am_origin && hout->in)
		return corrupt(abortstr, "Both origin and incoming");

	if (hout->in) {
		if (amount_msat_less(hout->in->msat, hout->msat))
			return corrupt(abortstr, "Input amount %s"
				       " less than %s",
				       type_to_string(tmpctx, struct amount_msat,
						      &hout->in->msat),
				       type_to_string(tmpctx, struct amount_msat,
						      &hout->msat));
		if (hout->in->cltv_expiry <= hout->cltv_expiry)
			return corrupt(abortstr, "Input cltv_expiry %u"
				       " less than %u",
				       hout->in->cltv_expiry, hout->cltv_expiry);
		if (!sha256_eq(&hout->in->payment_hash, &hout->payment_hash))
			return corrupt(abortstr, "Input hash != output hash");
		/* If output is resolved, input must be resolved same
		 * way (or not resolved yet). */
		if (hout->failuremsg) {
			if (hout->in->failcode)
				return corrupt(abortstr,
					       "Output failmsg, input failcode");
			if (hout->in->preimage)
				return corrupt(abortstr,
					       "Output failmsg, input preimage");
		} else if (hout->failcode) {
			if (hout->in->failuremsg)
				return corrupt(abortstr,
					       "Output failcode, input failmsg");
			if (hout->in->preimage)
				return corrupt(abortstr,
					       "Output failcode, input preimage");
		} else if (hout->preimage) {
			if (hout->in->failuremsg)
				return corrupt(abortstr,
					       "Output preimage, input failmsg");
			if (hout->in->failcode)
				return corrupt(abortstr,
					       "Output preimage, input failcode");
		} else {
			if (hout->in->preimage)
				return corrupt(abortstr,
					       "Output unresolved, input preimage");
			if (hout->in->failuremsg)
				return corrupt(abortstr,
					       "Output unresovled, input failmsg");
			if (hout->in->failcode)
				return corrupt(abortstr,
					       "Output unresolved, input failcode");
		}
	}

	/* Can't have a resolution while still being added. */
	if (hout->hstate >= SENT_ADD_HTLC
	    && hout->hstate <= SENT_ADD_ACK_REVOCATION) {
		if (hout->preimage)
			return corrupt(abortstr, "Still adding, has preimage");
		if (hout->failuremsg)
			return corrupt(abortstr, "Still adding, has failmsg");
		if (hout->failcode)
			return corrupt(abortstr, "Still adding, has failcode");
	} else if (hout->hstate >= RCVD_REMOVE_HTLC
		   && hout->hstate <= RCVD_REMOVE_ACK_REVOCATION) {
		if (!hout->preimage && !hout->failuremsg && !hout->failcode)
			return corrupt(abortstr, "Removing, no resolution");
	} else
		return corrupt(abortstr, "Bad state %s",
			       htlc_state_name(hout->hstate));

	return cast_const(struct htlc_out *, hout);
}

static void htlc_out_clear_hin(struct htlc_in *hin, struct htlc_out *hout)
{
	assert(hout->in == hin);
	hout->in = NULL;
}

static void destroy_htlc_out_with_hin(struct htlc_out *hout)
{
	/* Don't try to clear our ptr if we're freed before hin! */
	if (hout->in)
		tal_del_destructor2(hout->in, htlc_out_clear_hin, hout);
}

void htlc_out_connect_htlc_in(struct htlc_out *hout, struct htlc_in *hin)
{
	assert(!hout->in);
	hout->in = hin;
	tal_add_destructor2(hin, htlc_out_clear_hin, hout);
	tal_add_destructor(hout, destroy_htlc_out_with_hin);
}

/* You need to set the ID, then connect_htlc_out this! */
struct htlc_out *new_htlc_out(const tal_t *ctx,
			      struct channel *channel,
			      struct amount_msat msat,
			      u32 cltv_expiry,
			      u32 timestamp,
			      const struct sha256 *payment_hash,
			      const u8 *onion_routing_packet,
			      bool am_origin,
			      struct htlc_in *in)
{
	struct htlc_out *hout = tal(ctx, struct htlc_out);

        /* Mark this as an as of now unsaved HTLC */
	hout->dbid = 0;

	hout->key.channel = channel;
	hout->key.id = HTLC_INVALID_ID;
	hout->msat = msat;
	hout->cltv_expiry = cltv_expiry;
	hout->timestamp = timestamp;
	hout->payment_hash = *payment_hash;
	memcpy(hout->onion_routing_packet, onion_routing_packet,
	       sizeof(hout->onion_routing_packet));

	hout->hstate = SENT_ADD_HTLC;
	hout->failcode = 0;
	hout->failuremsg = NULL;
	hout->preimage = NULL;

	hout->am_origin = am_origin;
	hout->in = NULL;
	if (in)
		htlc_out_connect_htlc_in(hout, in);

	return htlc_out_check(hout, "new_htlc_out");
}
