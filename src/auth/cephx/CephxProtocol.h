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

#ifndef CEPH_CEPHXPROTOCOL_H
#define CEPH_CEPHXPROTOCOL_H

/*
  Ceph X protocol

  First, the principal has to authenticate with the authenticator. A
  shared-secret mechanism is being used, and the negotitaion goes like this:

  A = Authenticator
  P = Principle
  S = Service

  1. Obtaining principal/auth session key

  (Authenticate Request)
  p->a : principal, principal_addr.  authenticate me!

 ...authenticator does lookup in database...

  a->p : A= {principal/auth session key, validity}^principal_secret (*)
         B= {principal ticket, validity, principal/auth session key}^authsecret

  
  [principal/auth session key, validity] = service ticket
  [principal ticket, validity, principal/auth session key] = service ticket info

  (*) annotation: ^ signifies 'encrypted by'

  At this point, if is genuine, the principal should have the principal/auth
  session key at hand. The next step would be to request an authorization to
  use some other service:

  2. Obtaining principal/service session key

  p->a : B, {principal_addr, timestamp}^principal/auth session key.  authorize
         me!
  a->p : E= {service ticket}^svcsecret
         F= {principal/service session key, validity}^principal/auth session key

  principal_addr, timestamp = authenticator

  service ticket = principal name, client network address, validity, principal/service session key

  Note that steps 1 and 2 are pretty much the same thing; contacting the
  authenticator and requesting for a key.

  Following this the principal should have a principal/service session key that
  could be used later on for creating a session:

  3. Opening a session to a service

  p->s : E + {principal_addr, timestamp}^principal/service session key
  s->p : {timestamp+1}^principal/service/session key

  timestamp+1 = reply authenticator

  Now, the principal is fully authenticated with the service. So, logically we
  have 2 main actions here. The first one would be to obtain a session key to
  the service (steps 1 and 2), and the second one would be to authenticate with
  the service, using that ticket.
*/

/* authenticate requests */
#define CEPHX_GET_AUTH_SESSION_KEY      0x0100
#define CEPHX_GET_PRINCIPAL_SESSION_KEY 0x0200
#define CEPHX_GET_ROTATING_KEY          0x0400

#define CEPHX_REQUEST_TYPE_MASK            0x0F00
#define CEPHX_CRYPT_ERR			1

#include "../Auth.h"
#include "../RotatingKeyRing.h"
#include "common/debug.h"

#include <errno.h>
#include <sstream>

class CephContext;

/*
 * Authentication
 */

// initial server -> client challenge
struct CephXServerChallenge {
  uint64_t server_challenge;

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(server_challenge, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(server_challenge, bl);
  }
};
WRITE_CLASS_ENCODER(CephXServerChallenge)


// request/reply headers, for subsequent exchanges.

struct CephXRequestHeader {
  __u16 request_type;

  void encode(bufferlist& bl) const {
    ::encode(request_type, bl);
  }
  void decode(bufferlist::iterator& bl) {
    ::decode(request_type, bl);
  }
};
WRITE_CLASS_ENCODER(CephXRequestHeader)

struct CephXResponseHeader {
  uint16_t request_type;
  int32_t status;

  void encode(bufferlist& bl) const {
    ::encode(request_type, bl);
    ::encode(status, bl);
  }
  void decode(bufferlist::iterator& bl) {
    ::decode(request_type, bl);
    ::decode(status, bl);
  }
};
WRITE_CLASS_ENCODER(CephXResponseHeader)

struct CephXTicketBlob {
  uint64_t secret_id;
  bufferlist blob;

  CephXTicketBlob() : secret_id(0) {}

  void encode(bufferlist& bl) const {
     __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(secret_id, bl);
    ::encode(blob, bl);
  }

  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(secret_id, bl);
    ::decode(blob, bl);
  }
};
WRITE_CLASS_ENCODER(CephXTicketBlob)

// client -> server response to challenge
struct CephXAuthenticate {
  uint64_t client_challenge;
  uint64_t key;
  CephXTicketBlob old_ticket;

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(client_challenge, bl);
    ::encode(key, bl);
    ::encode(old_ticket, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(client_challenge, bl);
    ::decode(key, bl);
    ::decode(old_ticket, bl);
 }
};
WRITE_CLASS_ENCODER(CephXAuthenticate)

struct CephXChallengeBlob {
  uint64_t server_challenge, client_challenge;
  
  void encode(bufferlist& bl) const {
    ::encode(server_challenge, bl);
    ::encode(client_challenge, bl);
  }
  void decode(bufferlist::iterator& bl) {
    ::decode(server_challenge, bl);
    ::decode(client_challenge, bl);
  }
};
WRITE_CLASS_ENCODER(CephXChallengeBlob)

void cephx_calc_client_server_challenge(CephContext *cct, 
					CryptoKey& secret, uint64_t server_challenge, uint64_t client_challenge,
					uint64_t *key, std::string &error);


/*
 * getting service tickets
 */
struct CephXSessionAuthInfo {
  uint32_t service_id;
  uint64_t secret_id;
  AuthTicket ticket;
  CryptoKey session_key;
  CryptoKey service_secret;
  utime_t validity;
};


extern bool cephx_build_service_ticket_blob(CephContext *cct,
					    CephXSessionAuthInfo& ticket_info, CephXTicketBlob& blob);

extern void cephx_build_service_ticket_request(CephContext *cct, 
					       uint32_t keys,
					       bufferlist& request);

extern bool cephx_build_service_ticket_reply(CephContext *cct,
					     CryptoKey& principal_secret,
					     vector<CephXSessionAuthInfo> ticket_info,
                                             bool should_encrypt_ticket,
                                             CryptoKey& ticket_enc_key,
					     bufferlist& reply);

struct CephXServiceTicketRequest {
  uint32_t keys;

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(keys, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(keys, bl);
  }
};
WRITE_CLASS_ENCODER(CephXServiceTicketRequest)


/*
 * Authorize
 */

struct CephXAuthorizeReply {
  uint64_t nonce_plus_one;
  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(nonce_plus_one, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(nonce_plus_one, bl);
  }
};
WRITE_CLASS_ENCODER(CephXAuthorizeReply)


struct CephXAuthorizer : public AuthAuthorizer {
private:
  CephContext *cct;
public:
  uint64_t nonce;

  CephXAuthorizer(CephContext *cct_)
    : AuthAuthorizer(CEPH_AUTH_CEPHX), cct(cct_), nonce(0) {}

  bool build_authorizer();
  bool verify_reply(bufferlist::iterator& reply);
};



/*
 * TicketHandler
 */
struct CephXTicketHandler {
  uint32_t service_id;
  CryptoKey session_key;
  CephXTicketBlob ticket;        // opaque to us
  utime_t renew_after, expires;
  bool have_key_flag;

  CephXTicketHandler(CephContext *cct_, uint32_t service_id_)
    : service_id(service_id_), have_key_flag(false), cct(cct_) { }

  // to build our ServiceTicket
  bool verify_service_ticket_reply(CryptoKey& principal_secret,
				 bufferlist::iterator& indata);
  // to access the service
  CephXAuthorizer *build_authorizer(uint64_t global_id) const;

  bool have_key();
  bool need_key() const;

  void invalidate_ticket() {
    have_key_flag = 0;
  }
private:
  CephContext *cct;
};

struct CephXTicketManager {
  typedef map<uint32_t, CephXTicketHandler> tickets_map_t;
  tickets_map_t tickets_map;
  uint64_t global_id;

  CephXTicketManager(CephContext *cct_) : global_id(0), cct(cct_) {}

  bool verify_service_ticket_reply(CryptoKey& principal_secret,
				 bufferlist::iterator& indata);

  CephXTicketHandler& get_handler(uint32_t type) {
    tickets_map_t::iterator i = tickets_map.find(type);
    if (i != tickets_map.end())
      return i->second;
    CephXTicketHandler newTicketHandler(cct, type);
    std::pair < tickets_map_t::iterator, bool > res =
	tickets_map.insert(std::make_pair(type, newTicketHandler));
    assert(res.second);
    return res.first->second;
  }
  CephXAuthorizer *build_authorizer(uint32_t service_id) const;
  bool have_key(uint32_t service_id);
  bool need_key(uint32_t service_id) const;
  void set_have_need_key(uint32_t service_id, uint32_t& have, uint32_t& need);
  void validate_tickets(uint32_t mask, uint32_t& have, uint32_t& need);
  void invalidate_ticket(uint32_t service_id);

private:
  CephContext *cct;
};


/* A */
struct CephXServiceTicket {
  CryptoKey session_key;
  utime_t validity;

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(session_key, bl);
    ::encode(validity, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(session_key, bl);
    ::decode(validity, bl);
  }
};
WRITE_CLASS_ENCODER(CephXServiceTicket)

/* B */
struct CephXServiceTicketInfo {
  AuthTicket ticket;
  CryptoKey session_key;

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(ticket, bl);
    ::encode(session_key, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(ticket, bl);
    ::decode(session_key, bl);
  }
};
WRITE_CLASS_ENCODER(CephXServiceTicketInfo)

struct CephXAuthorize {
  uint64_t nonce;
  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(nonce, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(nonce, bl);
  }
};
WRITE_CLASS_ENCODER(CephXAuthorize)

/*
 * Decode an extract ticket
 */
bool cephx_decode_ticket(CephContext *cct, KeyStore *keys,
			 uint32_t service_id, CephXTicketBlob& ticket_blob,
			 CephXServiceTicketInfo& ticket_info);

/*
 * Verify authorizer and generate reply authorizer
 */
extern bool cephx_verify_authorizer(CephContext *cct, KeyStore *keys,
				    bufferlist::iterator& indata,
				    CephXServiceTicketInfo& ticket_info, bufferlist& reply_bl);






/*
 * encode+encrypt macros
 */
#define AUTH_ENC_MAGIC 0xff009cad8826aa55ull

template <typename T>
void decode_decrypt_enc_bl(CephContext *cct, T& t, CryptoKey key, bufferlist& bl_enc, 
			   std::string &error)
{
  uint64_t magic;
  bufferlist bl;

  key.decrypt(cct, bl_enc, bl, error);
  if (!error.empty())
    return;

  bufferlist::iterator iter2 = bl.begin();
  __u8 struct_v;
  ::decode(struct_v, iter2);
  ::decode(magic, iter2);
  if (magic != AUTH_ENC_MAGIC) {
    ostringstream oss;
    oss << "bad magic in decode_decrypt, " << magic << " != " << AUTH_ENC_MAGIC;
    error = oss.str();
    return;
  }

  ::decode(t, iter2);
}

template <typename T>
void encode_encrypt_enc_bl(CephContext *cct, const T& t, const CryptoKey& key,
			   bufferlist& out, std::string &error)
{
  bufferlist bl;
  __u8 struct_v = 1;
  ::encode(struct_v, bl);
  uint64_t magic = AUTH_ENC_MAGIC;
  ::encode(magic, bl);
  ::encode(t, bl);

  key.encrypt(cct, bl, out, error);
}

template <typename T>
int decode_decrypt(CephContext *cct, T& t, const CryptoKey& key,
		    bufferlist::iterator& iter, std::string &error)
{
  bufferlist bl_enc;
  try {
    ::decode(bl_enc, iter);
    decode_decrypt_enc_bl(cct, t, key, bl_enc, error);
  }
  catch (buffer::error &e) {
    error = "error decoding block for decryption";
  }
  if (!error.empty())
    return CEPHX_CRYPT_ERR;
  return 0;
}

template <typename T>
int encode_encrypt(CephContext *cct, const T& t, const CryptoKey& key,
		    bufferlist& out, std::string &error)
{
  bufferlist bl_enc;
  encode_encrypt_enc_bl(cct, t, key, bl_enc, error);
  if (!error.empty()){
    return CEPHX_CRYPT_ERR;
  }
  ::encode(bl_enc, out);
  return 0;
}


#endif
