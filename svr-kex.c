/*
 * Dropbear - a SSH2 server
 * 
 * Copyright (c) 2002,2003 Matt Johnston
 * Copyright (c) 2004 by Mihnea Stoenescu
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#include "includes.h"
#include "dbutil.h"
#include "algo.h"
#include "buffer.h"
#include "session.h"
#include "kex.h"
#include "ssh.h"
#include "packet.h"
#include "bignum.h"
#include "random.h"
#include "runopts.h"
#include "ecc.h"
#include "gensignkey.h"

static void send_msg_kexdh_reply(mp_int *dh_e, buffer *ecdh_qs);

/* Handle a diffie-hellman key exchange initialisation. This involves
 * calculating a session key reply value, and corresponding hash. These
 * are carried out by send_msg_kexdh_reply(). recv_msg_kexdh_init() calls
 * that function, then brings the new keys into use */
void recv_msg_kexdh_init() {

	DEF_MP_INT(dh_e);
	buffer *ecdh_qs = NULL;

	TRACE(("enter recv_msg_kexdh_init"))
	if (!ses.kexstate.recvkexinit) {
		dropbear_exit("Premature kexdh_init message received");
	}

	if (IS_NORMAL_DH(ses.newkeys->algo_kex)) {
		m_mp_init(&dh_e);
		if (buf_getmpint(ses.payload, &dh_e) != DROPBEAR_SUCCESS) {
			dropbear_exit("Failed to get kex value");
		}
	} else {
#ifdef DROPBEAR_ECDH
		ecdh_qs = buf_getstringbuf(ses.payload);
#endif
	}

	send_msg_kexdh_reply(&dh_e, ecdh_qs);

	mp_clear(&dh_e);
	if (ecdh_qs) {
		buf_free(ecdh_qs);
	}

	send_msg_newkeys();
	ses.requirenext[0] = SSH_MSG_NEWKEYS;
	ses.requirenext[1] = 0;
	TRACE(("leave recv_msg_kexdh_init"))
}

static void svr_ensure_hostkey() {

	const char* fn = NULL;
	char *fn_temp = NULL;
	enum signkey_type type = ses.newkeys->algo_hostkey;
	void **hostkey = signkey_key_ptr(svr_opts.hostkey, type);
	int ret = DROPBEAR_FAILURE;

	if (hostkey && *hostkey) {
		return;
	}

	switch (type)
	{
#ifdef DROPBEAR_RSA
		case DROPBEAR_SIGNKEY_RSA:
			fn = RSA_PRIV_FILENAME;
			break;
#endif
#ifdef DROPBEAR_DSS
		case DROPBEAR_SIGNKEY_DSS:
			fn = DSS_PRIV_FILENAME;
			break;
#endif
#ifdef DROPBEAR_ECDSA
		case DROPBEAR_SIGNKEY_ECDSA_NISTP256:
		case DROPBEAR_SIGNKEY_ECDSA_NISTP384:
		case DROPBEAR_SIGNKEY_ECDSA_NISTP521:
			fn = ECDSA_PRIV_FILENAME;
			break;
#endif
		default:
			(void)0;
	}

	if (readhostkey(fn, svr_opts.hostkey, &type) == DROPBEAR_SUCCESS) {
		return;
	}

	fn_temp = m_malloc(strlen(fn) + 20);
	snprintf(fn_temp, strlen(fn)+20, "%s.tmp%d", fn, getpid());

	if (signkey_generate(type, 0, fn_temp) == DROPBEAR_FAILURE) {
		goto out;
	}

	if (link(fn_temp, fn) < 0) {
		if (errno != EEXIST) {
			dropbear_log(LOG_ERR, "Failed moving key file to %s", fn);
			/* XXX fallback to non-atomic copy for some filesystems? */
			goto out;
		}
	}

	ret = readhostkey(fn, svr_opts.hostkey, &type);

out:
	if (fn_temp) {
		unlink(fn_temp);
		m_free(fn_temp);
	}

	if (ret == DROPBEAR_FAILURE)
	{
		dropbear_exit("Couldn't read or generate hostkey");
	}

	// directory for keys.

	// Create lockfile first, or wait if it exists. PID!
	// Generate key
	// write it, load to memory
	// atomic rename, done.

}
	
/* Generate our side of the diffie-hellman key exchange value (dh_f), and
 * calculate the session key using the diffie-hellman algorithm. Following
 * that, the session hash is calculated, and signed with RSA or DSS. The
 * result is sent to the client. 
 *
 * See the transport RFC4253 section 8 for details
 * or RFC5656 section 4 for elliptic curve variant. */
static void send_msg_kexdh_reply(mp_int *dh_e, buffer *ecdh_qs) {
	TRACE(("enter send_msg_kexdh_reply"))

	/* we can start creating the kexdh_reply packet */
	CHECKCLEARTOWRITE();
	
	svr_ensure_hostkey();

	buf_putbyte(ses.writepayload, SSH_MSG_KEXDH_REPLY);
	buf_put_pub_key(ses.writepayload, svr_opts.hostkey,
			ses.newkeys->algo_hostkey);

	if (IS_NORMAL_DH(ses.newkeys->algo_kex)) {
		// Normal diffie-hellman
		struct kex_dh_param * dh_param = gen_kexdh_param();
		kexdh_comb_key(dh_param, dh_e, svr_opts.hostkey);

		/* put f */
		buf_putmpint(ses.writepayload, &dh_param->pub);
		free_kexdh_param(dh_param);
	} else {
#ifdef DROPBEAR_ECDH
		struct kex_ecdh_param *ecdh_param = gen_kexecdh_param();
		kexecdh_comb_key(ecdh_param, ecdh_qs, svr_opts.hostkey);

		buf_put_ecc_raw_pubkey_string(ses.writepayload, &ecdh_param->key);
		free_kexecdh_param(ecdh_param);
#endif
	}

	/* calc the signature */
	buf_put_sign(ses.writepayload, svr_opts.hostkey, 
			ses.newkeys->algo_hostkey, ses.hash);

	/* the SSH_MSG_KEXDH_REPLY is done */
	encrypt_packet();

	TRACE(("leave send_msg_kexdh_reply"))
}

