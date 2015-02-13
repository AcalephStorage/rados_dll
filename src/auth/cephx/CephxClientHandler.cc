// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include <errno.h>

#include "CephxClientHandler.h"
#include "CephxProtocol.h"

#include "../KeyRing.h"

#include "common/config.h"

#define dout_subsys ceph_subsys_auth
#undef dout_prefix
#define dout_prefix *_dout << "cephx client: "


int CephxClientHandler::build_request(bufferlist& bl) const
{
  ldout(cct, 10) << "build_request" << dendl;

  RWLock::RLocker l(lock);

  if (need & CEPH_ENTITY_TYPE_AUTH) {
    /* authenticate */
    CephXRequestHeader header;
    header.request_type = CEPHX_GET_AUTH_SESSION_KEY;
    ::encode(header, bl);

    CryptoKey secret;
    keyring->get_secret(cct->_conf->name, secret);

    CephXAuthenticate req;
    get_random_bytes((char *)&req.client_challenge, sizeof(req.client_challenge));
    std::string error;
    cephx_calc_client_server_challenge(cct, secret, server_challenge,
				       req.client_challenge, &req.key, error);
    if (!error.empty()) {
      ldout(cct, 20) << "cephx_calc_client_server_challenge error: " << error << dendl;
      return -EIO;
    }

    req.old_ticket = ticket_handler->ticket;

    if (req.old_ticket.blob.length()) {
      ldout(cct, 20) << "old ticket len=" << req.old_ticket.blob.length() << dendl;
    }

    ::encode(req, bl);

    ldout(cct, 10) << "get auth session key: client_challenge " << req.client_challenge << dendl;
    return 0;
  }

  if (need) {
    /* get service tickets */
    ldout(cct, 10) << "get service keys: want=" << want << " need=" << need << " have=" << have << dendl;

    CephXRequestHeader header;
    header.request_type = CEPHX_GET_PRINCIPAL_SESSION_KEY;
    ::encode(header, bl);

    CephXAuthorizer *authorizer = ticket_handler->build_authorizer(global_id);
    if (!authorizer)
      return -EINVAL;
    bl.claim_append(authorizer->bl);
    delete authorizer;

    CephXServiceTicketRequest req;
    req.keys = need;
    ::encode(req, bl);
  }

  return 0;
}

int CephxClientHandler::handle_response(int ret, bufferlist::iterator& indata)
{
  ldout(cct, 10) << "handle_response ret = " << ret << dendl;
  RWLock::WLocker l(lock);
  
  if (ret < 0)
    return ret; // hrm!

  if (starting) {
    CephXServerChallenge ch;
    ::decode(ch, indata);
    server_challenge = ch.server_challenge;
    ldout(cct, 10) << " got initial server challenge " << server_challenge << dendl;
    starting = false;

    tickets.invalidate_ticket(CEPH_ENTITY_TYPE_AUTH);
    return -EAGAIN;
  }

  struct CephXResponseHeader header;
  ::decode(header, indata);

  switch (header.request_type) {
  case CEPHX_GET_AUTH_SESSION_KEY:
    {
      ldout(cct, 10) << " get_auth_session_key" << dendl;
      CryptoKey secret;
      keyring->get_secret(cct->_conf->name, secret);
	
      if (!tickets.verify_service_ticket_reply(secret, indata)) {
	ldout(cct, 0) << "could not verify service_ticket reply" << dendl;
	return -EPERM;
      }
      ldout(cct, 10) << " want=" << want << " need=" << need << " have=" << have << dendl;
      validate_tickets();
      if (need)
	ret = -EAGAIN;
      else
	ret = 0;
    }
    break;

  case CEPHX_GET_PRINCIPAL_SESSION_KEY:
    {
      CephXTicketHandler& ticket_handler = tickets.get_handler(CEPH_ENTITY_TYPE_AUTH);
      ldout(cct, 10) << " get_principal_session_key session_key " << ticket_handler.session_key << dendl;
  
      if (!tickets.verify_service_ticket_reply(ticket_handler.session_key, indata)) {
        ldout(cct, 0) << "could not verify service_ticket reply" << dendl;
        return -EPERM;
      }
      validate_tickets();
      if (!need) {
	ret = 0;
      }
    }
    break;

  case CEPHX_GET_ROTATING_KEY:
    {
      ldout(cct, 10) << " get_rotating_key" << dendl;
      if (rotating_secrets) {
	RotatingSecrets secrets;
	CryptoKey secret_key;
	keyring->get_secret(cct->_conf->name, secret_key);
	std::string error;
	if (decode_decrypt(cct, secrets, secret_key, indata, error)) {
	  ldout(cct, 0) << "could not set rotating key: decode_decrypt failed. error:"
	    << error << dendl;
	  error.clear();
	} else {
	  rotating_secrets->set_secrets(secrets);
	}
      }
    }
    break;

  default:
   ldout(cct, 0) << " unknown request_type " << header.request_type << dendl;
   assert(0);
  }
  return ret;
}



AuthAuthorizer *CephxClientHandler::build_authorizer(uint32_t service_id) const
{
  RWLock::RLocker l(lock);
  ldout(cct, 10) << "build_authorizer for service " << ceph_entity_type_name(service_id) << dendl;
  return tickets.build_authorizer(service_id);
}


bool CephxClientHandler::build_rotating_request(bufferlist& bl) const
{
  ldout(cct, 10) << "build_rotating_request" << dendl;
  CephXRequestHeader header;
  header.request_type = CEPHX_GET_ROTATING_KEY;
  ::encode(header, bl);
  return true;
}

void CephxClientHandler::prepare_build_request()
{
  RWLock::WLocker l(lock);
  ldout(cct, 10) << "validate_tickets: want=" << want << " need=" << need
		 << " have=" << have << dendl;
  validate_tickets();
  ldout(cct, 10) << "want=" << want << " need=" << need << " have=" << have
		 << dendl;

  ticket_handler = &(tickets.get_handler(CEPH_ENTITY_TYPE_AUTH));
}

void CephxClientHandler::validate_tickets()
{
  // lock should be held for write
  tickets.validate_tickets(want, have, need);
}

bool CephxClientHandler::need_tickets()
{
  RWLock::WLocker l(lock);
  validate_tickets();

  ldout(cct, 20) << "need_tickets: want=" << want << " need=" << need << " have=" << have << dendl;

  return (need != 0);
}

