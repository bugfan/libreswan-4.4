/*
 * Copyright (C) 2007-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2011-2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012-2018 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012,2016-2017 Antony Antony <appu@phenome.org>
 * Copyright (C) 2013-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2014-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Antony Antony <antony@phenome.org>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "sysdep.h"
#include "constants.h"

#include "defs.h"
#include "id.h"
#include "x509.h"
#include "pluto_x509.h"
#include "certs.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "packet.h"
#include "crypto.h"
#include "ike_alg.h"
#include "log.h"
#include "demux.h"      /* needs packet.h */
#include "ikev2.h"
#include "ipsec_doi.h"  /* needs demux.h and state.h */
#include "timer.h"
#include "whack.h"      /* requires connections.h */
#include "server.h"
#include "vendor.h"
#include "kernel.h"
#include "host_pair.h"
#include "addresspool.h"
#include "rnd.h"
#include "ip_address.h"
#include "ikev2_send.h"
#include "ikev2_message.h"
#include "ikev2_ts.h"
#include "ip_info.h"
#ifdef USE_XFRM_INTERFACE
# include "kernel_xfrm_interface.h"
#endif

stf_status ikev2_child_sa_respond(struct ike_sa *ike,
				  struct child_sa *child,
				  struct msg_digest *md,
				  pb_stream *outpbs,
				  enum isakmp_xchg_types isa_xchg)
{
	struct state *cst = &child->sa;	/* child state */

	/* switch to child */
	pexpect(md->st == &child->sa);
	struct connection *c = cst->st_connection;

	if (c->spd.that.has_lease &&
	    md->chain[ISAKMP_NEXT_v2CP] != NULL &&
	    child->sa.st_state->kind != STATE_V2_REKEY_IKE_R0) {
		/*
		 * XXX: should this be passed the CHILD SA's
		 * .st_connection?  Here things are negotiating a new
		 * CHILD?
		 */
		if (!emit_v2_child_configuration_payload(ike->sa.st_connection,
							 child, outpbs)) {
			return STF_INTERNAL_ERROR;
		}
	} else if (md->chain[ISAKMP_NEXT_v2CP] != NULL) {
		dbg("#%lu %s ignoring unexpected v2CP payload",
		    cst->st_serialno, cst->st_state->name);
	}

	/* start of SA out */
	{
		/* ??? this code won't support AH + ESP */
		struct ipsec_proto_info *proto_info
			= ikev2_child_sa_proto_info(pexpect_child_sa(cst), c->policy);

		if (isa_xchg != ISAKMP_v2_CREATE_CHILD_SA)  {
			stf_status res = ikev2_process_child_sa_pl(ike, child, md, FALSE);
			if (res != STF_OK)
				return res;
		}
		proto_info->our_spi = ikev2_child_sa_spi(&c->spd, c->policy,
							 child->sa.st_logger);
		chunk_t local_spi = THING_AS_CHUNK(proto_info->our_spi);
		if (!ikev2_emit_sa_proposal(outpbs,
					    child->sa.st_accepted_esp_or_ah_proposal,
					    &local_spi)) {
			dbg("problem emitting accepted proposal");
			return STF_INTERNAL_ERROR;
		}
	}

	if (isa_xchg == ISAKMP_v2_CREATE_CHILD_SA) {
		/* send NONCE */
		struct ikev2_generic in = {
			.isag_critical = build_ikev2_critical(false, ike->sa.st_logger),
		};
		pb_stream pb_nr;
		if (!out_struct(&in, &ikev2_nonce_desc, outpbs, &pb_nr) ||
		    !out_hunk(cst->st_nr, &pb_nr, "IKEv2 nonce"))
			return STF_INTERNAL_ERROR;

		close_output_pbs(&pb_nr);

		/*
		 * XXX: shouldn't this be conditional on the local end
		 * having computed KE and not what the remote sent?
		 */
		if (md->chain[ISAKMP_NEXT_v2KE] != NULL)  {
			if (!emit_v2KE(&cst->st_gr, cst->st_oakley.ta_dh, outpbs))
				return STF_INTERNAL_ERROR;
		}
	}

	/*
	 * Paul: This is the second time we are processing NOTIFY's I
	 * suspect we are only interested in those related to the
	 * Child SA and mark those on the child state. But this code
	 * is used in IKE_AUTH as well as CREATE_CHILD_SA, so we end
	 * up double logging bad payloads on the responder.
	 *
	 * XXX: all these notifications have already been decoded and
	 * can be found in md->pbs[].
	 */
	/* Process all NOTIFY payloads */
	for (struct payload_digest *ntfy = md->chain[ISAKMP_NEXT_v2N];
	     ntfy != NULL; ntfy = ntfy->next) {
		switch (ntfy->payload.v2n.isan_type) {
		case v2N_NAT_DETECTION_SOURCE_IP:
		case v2N_NAT_DETECTION_DESTINATION_IP:
		case v2N_IKEV2_FRAGMENTATION_SUPPORTED:
		case v2N_COOKIE:
		case v2N_USE_PPK:
		case v2N_INTERMEDIATE_EXCHANGE_SUPPORTED:
			dbg("received %s which is not valid for current exchange",
			    enum_name(&ikev2_notify_names, ntfy->payload.v2n.isan_type));
			break;
		case v2N_USE_TRANSPORT_MODE:
			dbg("received USE_TRANSPORT_MODE");
			cst->st_seen_use_transport = TRUE;
			break;
		case v2N_IPCOMP_SUPPORTED:
		{
			pb_stream pbs = ntfy->pbs;
			size_t len = pbs_left(&pbs);
			struct ikev2_notify_ipcomp_data n_ipcomp;

			dbg("received v2N_IPCOMP_SUPPORTED of length %zd", len);

			diag_t d = pbs_in_struct(&pbs, &ikev2notify_ipcomp_data_desc,
						 &n_ipcomp, sizeof(n_ipcomp), NULL);
			if (d != NULL) {
				llog_diag(RC_LOG, child->sa.st_logger, &d, "%s", "");
				return STF_FATAL;
			}

			if (n_ipcomp.ikev2_notify_ipcomp_trans != IPCOMP_DEFLATE) {
				log_state(RC_LOG_SERIOUS, &child->sa,
					  "Unsupported IPCOMP compression method %d",
					  n_ipcomp.ikev2_notify_ipcomp_trans); /* enum_name this later */
				return STF_FATAL;
			}
			if (n_ipcomp.ikev2_cpi < IPCOMP_FIRST_NEGOTIATED) {
				log_state(RC_LOG_SERIOUS, &child->sa,
					  "Illegal IPCOMP CPI %d", n_ipcomp.ikev2_cpi);
				return STF_FATAL;
			}
			if ((c->policy & POLICY_COMPRESS) == LEMPTY) {
				dbg("Ignored IPCOMP request as connection has compress=no");
				cst->st_ipcomp.present = FALSE;
				break;
			}
			dbg("Received compression CPI=%d", htonl(n_ipcomp.ikev2_cpi));

			//cst->st_ipcomp.attrs.spi = uniquify_peer_cpi((ipsec_spi_t)htonl(n_ipcomp.ikev2_cpi), cst, 0);
			cst->st_ipcomp.attrs.spi = htonl((ipsec_spi_t)n_ipcomp.ikev2_cpi);
			cst->st_ipcomp.attrs.transattrs.ta_comp = n_ipcomp.ikev2_notify_ipcomp_trans;
			cst->st_ipcomp.attrs.mode = ENCAPSULATION_MODE_TUNNEL; /* always? */
			cst->st_ipcomp.present = TRUE;
			cst->st_seen_use_ipcomp = TRUE;
			break;
		}
		case v2N_ESP_TFC_PADDING_NOT_SUPPORTED:
			dbg("received ESP_TFC_PADDING_NOT_SUPPORTED");
			cst->st_seen_no_tfc = TRUE;
			break;
		case v2N_MOBIKE_SUPPORTED:
			dbg("received v2N_MOBIKE_SUPPORTED");
			cst->st_seen_mobike = ike->sa.st_seen_mobike = TRUE;
			break;
		case v2N_INITIAL_CONTACT:
			dbg("received v2N_INITIAL_CONTACT");
			cst->st_seen_initialc = ike->sa.st_seen_initialc = TRUE;
			break;
		case v2N_REKEY_SA:
			dbg("received REKEY_SA already proceesd");
			break;
		case v2N_PPK_IDENTITY:
			dbg("received PPK_IDENTITY already processed");
			break;
		case v2N_NO_PPK_AUTH:
			dbg("received NO_PPK_AUTH already processed");
			break;
		default:
			log_state(RC_LOG, &child->sa,
				  "received unsupported NOTIFY %s ",
				  enum_name(&ikev2_notify_names, ntfy->payload.v2n.isan_type));
		}
	}

	{
		/* verify if transport / tunnel mode is matches */
		if ((c->policy & POLICY_TUNNEL) == LEMPTY) {
			/* we should have received transport mode request - and send one */
			if (!cst->st_seen_use_transport) {
				log_state(RC_LOG, &child->sa,
					  "policy dictates Transport Mode, but peer requested Tunnel Mode");
				return STF_FAIL + v2N_NO_PROPOSAL_CHOSEN;
			}
		} else {
			if (cst->st_seen_use_transport) {
				/* RFC allows us to ignore their (wrong) request for transport mode */
				log_state(RC_LOG, &child->sa,
					  "policy dictates Tunnel Mode, ignoring peer's request for Transport Mode");
			}
		}

		if (c->policy & POLICY_COMPRESS) {
			if (!cst->st_seen_use_ipcomp) {
				dbg("policy suggested compression, but peer did not offer support");
			}
		} else {
			if (cst->st_seen_use_ipcomp) {
				dbg("policy did not allow compression, ignoring peer's request");
			}
		}

		/*
		 * XXX: see above notes on 'role' - this must be the
		 * SA_RESPONDER.
		 */
		stf_status ret = v2_emit_ts_payloads(pexpect_child_sa(cst),
						     outpbs, c);

		if (ret != STF_OK)
			return ret;	/* should we delete_state cst? */
	}

	if (cst->st_seen_use_transport) {
		if (c->policy & POLICY_TUNNEL) {
			log_state(RC_LOG, &child->sa,
				  "Local policy is tunnel mode - ignoring request for transport mode");
		} else {
			dbg("Local policy is transport mode and received USE_TRANSPORT_MODE");
			if (cst->st_esp.present) {
				cst->st_esp.attrs.mode =
					ENCAPSULATION_MODE_TRANSPORT;
			}
			if (cst->st_ah.present) {
				cst->st_ah.attrs.mode =
					ENCAPSULATION_MODE_TRANSPORT;
			}
			/* In v2, for parent, protoid must be 0 and SPI must be empty */
			if (!emit_v2N(v2N_USE_TRANSPORT_MODE, outpbs))
				return STF_INTERNAL_ERROR;
		}
	} else {
		/* the peer wants tunnel mode */
		if ((c->policy & POLICY_TUNNEL) == LEMPTY) {
			log_state(RC_LOG_SERIOUS, &child->sa,
				  "Local policy is transport mode, but peer did not request that");
			return STF_FAIL + v2N_NO_PROPOSAL_CHOSEN;
		}
	}

	if (c->send_no_esp_tfc) {
		dbg("Sending ESP_TFC_PADDING_NOT_SUPPORTED");
		if (!emit_v2N(v2N_ESP_TFC_PADDING_NOT_SUPPORTED, outpbs))
			return STF_INTERNAL_ERROR;
	}

	if (!emit_v2N_compression(cst, cst->st_seen_use_ipcomp, outpbs))
		return STF_INTERNAL_ERROR;

	ikev2_derive_child_keys(pexpect_child_sa(cst));

	/*
	 * Check to see if we need to release an old instance
	 * Note that this will call delete on the old connection
	 * we should do this after installing ipsec_sa, but that will
	 * give us a "eroute in use" error.
	 */
	if (isa_xchg == ISAKMP_v2_CREATE_CHILD_SA) {
		/* skip check for rekey */
		ike->sa.st_connection->newest_isakmp_sa = ike->sa.st_serialno;
	} else {
#ifdef USE_XFRM_INTERFACE
		if (c->xfrmi != NULL && c->xfrmi->if_id != 0)
			if (add_xfrmi(c, child->sa.st_logger))
				return STF_FATAL;
#endif
		IKE_SA_established(ike);
	}

	/* install inbound and outbound SPI info */
	if (!install_ipsec_sa(cst, TRUE))
		return STF_FATAL;

	/* mark the connection as now having an IPsec SA associated with it. */
	set_newest_ipsec_sa(enum_name(&ikev2_exchange_names, isa_xchg), cst);

	return STF_OK;
}

static void ikev2_set_domain(pb_stream *cp_a_pbs, struct state *st)
{
	bool responder = (st->st_state->kind != STATE_PARENT_I2);
	bool ignore = LIN(POLICY_IGNORE_PEER_DNS, st->st_connection->policy);

	if (!responder) {
		char *safestr = cisco_stringify(cp_a_pbs, "INTERNAL_DNS_DOMAIN",
						ignore, st->st_logger);
		if (safestr != NULL) {
			append_st_cfg_domain(st, safestr);
		}
	} else {
		log_state(RC_LOG, st, "initiator INTERNAL_DNS_DOMAIN CP ignored");
	}
}

static bool ikev2_set_dns(pb_stream *cp_a_pbs, struct state *st,
			  const struct ip_info *af)
{
	struct connection *c = st->st_connection;
	bool ignore = LIN(POLICY_IGNORE_PEER_DNS, st->st_connection->policy);

	if (c->policy & POLICY_OPPORTUNISTIC) {
		log_state(RC_LOG, st, "ignored INTERNAL_IP%d_DNS CP payload for Opportunistic IPsec",
			  af->ip_version);
		return true;
	}

	ip_address ip;
	diag_t d = pbs_in_address(cp_a_pbs, &ip, af, "INTERNAL_IP_DNS CP payload");
	if (d != NULL) {
		llog_diag(RC_LOG, st->st_logger, &d, "%s", "");
		return false;
	}

	/* i.e. all zeros */
	if (address_is_any(ip)) {
		address_buf ip_str;
		log_state(RC_LOG, st, "ERROR INTERNAL_IP%d_DNS %s is invalid",
			  af->ip_version, ipstr(&ip, &ip_str));
		return false;
	}

	bool responder = (st->st_state->kind != STATE_PARENT_I2);
	if (!responder) {
		address_buf ip_buf;
		const char *ip_str = ipstr(&ip, &ip_buf);

		log_state(RC_LOG, st, "received %sINTERNAL_IP%d_DNS %s",
			  ignore ? "and ignored " : "",
			  af->ip_version, ip_str);
		if (!ignore)
			append_st_cfg_dns(st, ip_str);
	} else {
		log_state(RC_LOG, st, "initiator INTERNAL_IP%d_DNS CP ignored",
			  af->ip_version);
	}

	return true;
}

static bool ikev2_set_ia(pb_stream *cp_a_pbs, struct state *st,
			 const struct ip_info *af, bool *seen_an_address)
{
	struct connection *c = st->st_connection;

	ip_address ip;
	diag_t d = pbs_in_address(cp_a_pbs, &ip, af, "INTERNAL_IP_ADDRESS");
	if (d != NULL) {
		llog_diag(RC_LOG, st->st_logger, &d, "%s", "");
		return false;
	}

	/*
	 * if (af->af == AF_INET6) pbs_in_address only reads 16 bytes.
	 * There should be one more byte in the pbs, 17th byte is prefix length.
	 */

	if (address_is_any(ip)) {
		ipstr_buf ip_str;
		log_state(RC_LOG, st, "ERROR INTERNAL_IP%d_ADDRESS %s is invalid",
			  af->ip_version, ipstr(&ip, &ip_str));
		return false;
	}

	ipstr_buf ip_str;
	log_state(RC_LOG, st, "received INTERNAL_IP%d_ADDRESS %s%s",
		  af->ip_version, ipstr(&ip, &ip_str),
		  *seen_an_address ? "; discarded" : "");


	bool responder = st->st_state->kind != STATE_PARENT_I2;
	if (responder) {
		log_state(RC_LOG, st, "bogus responder CP ignored");
		return true;
	}

	if (*seen_an_address) {
		return true;
	}

	*seen_an_address = true;
	c->spd.this.has_client = true;
	c->spd.this.has_internal_address = true;

	if (c->spd.this.cat) {
		dbg("CAT is set, not setting host source IP address to %s",
		    ipstr(&ip, &ip_str));
		ip_address this_client_prefix = selector_prefix(c->spd.this.client);
		if (address_eq_address(this_client_prefix, ip)) {
			/*
			 * The address we received is same as this
			 * side should we also check the host_srcip.
			 */
			dbg("#%lu %s[%lu] received INTERNAL_IP%d_ADDRESS that is same as this.client.addr %s. Will not add CAT iptable rules",
			    st->st_serialno, c->name, c->instance_serial,
			    af->ip_version, ipstr(&ip, &ip_str));
		} else {
			c->spd.this.client = selector_from_address(ip);
			st->st_ts_this = ikev2_end_to_ts(&c->spd.this, st);
			c->spd.this.has_cat = true; /* create iptable entry */
		}
	} else {
		c->spd.this.client = selector_from_address(ip);
		/* only set sourceip= value if unset in configuration */
		if (address_is_unset(&c->spd.this.host_srcip) ||
		    address_is_any(c->spd.this.host_srcip)) {
			dbg("setting host source IP address to %s",
			    ipstr(&ip, &ip_str));
			c->spd.this.host_srcip = ip;
		}
	}

	return true;
}

bool ikev2_parse_cp_r_body(struct payload_digest *cp_pd, struct state *st)
{
	struct ikev2_cp *cp =  &cp_pd->payload.v2cp;
	struct connection *c = st->st_connection;
	pb_stream *attrs = &cp_pd->pbs;

	dbg("#%lu %s[%lu] parsing ISAKMP_NEXT_v2CP payload",
	    st->st_serialno, c->name, c->instance_serial);

	if (st->st_state->kind == STATE_PARENT_I2 && cp->isacp_type !=  IKEv2_CP_CFG_REPLY) {
		log_state(RC_LOG_SERIOUS, st,
			  "ERROR expected IKEv2_CP_CFG_REPLY got a %s",
			  enum_name(&ikev2_cp_type_names, cp->isacp_type));
		return FALSE;
	}

	if (st->st_state->kind == STATE_PARENT_R1 && cp->isacp_type !=  IKEv2_CP_CFG_REQUEST) {
		log_state(RC_LOG_SERIOUS, st,
			  "ERROR expected IKEv2_CP_CFG_REQUEST got a %s",
			  enum_name(&ikev2_cp_type_names, cp->isacp_type));
		return FALSE;
	}

	bool seen_internal_address = false;
	while (pbs_left(attrs) > 0) {
		struct ikev2_cp_attribute cp_a;
		pb_stream cp_a_pbs;

		diag_t d = pbs_in_struct(attrs, &ikev2_cp_attribute_desc,
					 &cp_a, sizeof(cp_a), &cp_a_pbs);
		if (d != NULL) {
			llog_diag(RC_LOG_SERIOUS, st->st_logger, &d,
				 "ERROR malformed CP attribute");
			return false;
		}

		switch (cp_a.type) {
		case IKEv2_INTERNAL_IP4_ADDRESS | ISAKMP_ATTR_AF_TLV:
			if (!ikev2_set_ia(&cp_a_pbs, st, &ipv4_info,
					  &seen_internal_address)) {
				log_state(RC_LOG_SERIOUS, st,
					  "ERROR malformed INTERNAL_IP4_ADDRESS attribute");
				return FALSE;
			}
			break;

		case IKEv2_INTERNAL_IP4_DNS | ISAKMP_ATTR_AF_TLV:
			if (!ikev2_set_dns(&cp_a_pbs, st, &ipv4_info)) {
				log_state(RC_LOG_SERIOUS, st,
					  "ERROR malformed INTERNAL_IP4_DNS attribute");
				return FALSE;
			}
			break;

		case IKEv2_INTERNAL_IP6_ADDRESS | ISAKMP_ATTR_AF_TLV:
			if (!ikev2_set_ia(&cp_a_pbs, st, &ipv6_info,
						 &seen_internal_address)) {
				log_state(RC_LOG_SERIOUS, st,
					  "ERROR malformed INTERNAL_IP6_ADDRESS attribute");
				return FALSE;
			}
			break;

		case IKEv2_INTERNAL_IP6_DNS | ISAKMP_ATTR_AF_TLV:
			if (!ikev2_set_dns(&cp_a_pbs, st, &ipv6_info)) {
				log_state(RC_LOG_SERIOUS, st,
					  "ERROR malformed INTERNAL_IP6_DNS attribute");
				return FALSE;
			}
			break;

		case IKEv2_INTERNAL_DNS_DOMAIN | ISAKMP_ATTR_AF_TLV:
			ikev2_set_domain(&cp_a_pbs, st); /* can't fail */
			break;

		default:
			log_state(RC_LOG, st,
				  "unknown attribute %s length %u",
				  enum_name(&ikev2_cp_attribute_type_names, cp_a.type),
				  cp_a.len);
			break;
		}
	}
	return TRUE;
}
