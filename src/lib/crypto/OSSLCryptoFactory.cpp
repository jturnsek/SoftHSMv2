/*
 * Copyright (c) 2010 SURFnet bv
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*****************************************************************************
 OSSLCryptoFactory.cpp

 This is an OpenSSL based cryptographic algorithm factory
 *****************************************************************************/

#include "config.h"
#include "MutexFactory.h"
#include "OSSLCryptoFactory.h"
#include "OSSLRNG.h"
#include "OSSLAES.h"
#include "OSSLDES.h"
#include "OSSLMD5.h"
#include "OSSLSHA1.h"
#include "OSSLSHA224.h"
#include "OSSLSHA256.h"
#include "OSSLSHA384.h"
#include "OSSLSHA512.h"
#include "OSSLCMAC.h"
#include "OSSLHMAC.h"
#include "OSSLRSA.h"
#include "OSSLDSA.h"
#include "OSSLDH.h"
#ifdef WITH_ECC
#include "OSSLECDH.h"
#include "OSSLECDSA.h"
#endif
#ifdef WITH_GOST
#include "OSSLGOSTR3411.h"
#include "OSSLGOST.h"
#endif
#ifdef WITH_EDDSA
#include "OSSLEDDSA.h"
#endif

#include <algorithm>
#include <string.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#ifdef WITH_GOST
#include <openssl/objects.h>
#endif

#include <dlfcn.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef WITH_FIPS
// Initialise the FIPS 140-2 selftest status
bool OSSLCryptoFactory::FipsSelfTestStatus = false;
#endif

static unsigned nlocks;
static Mutex** locks;

#define USE_TPM
#ifdef USE_TPM
static void *handle;
static const TSS2_TCTI_INFO *info;
static TSS2_TCTI_CONTEXT *tcti = NULL;

#define DISABLE_DLCLOSE

#define TSS2_TCTI_SO_FORMAT "libtss2-tcti-%s.so.0"

void tpm2_tcti_ldr_unload(void) {
  if (handle) {
#ifndef DISABLE_DLCLOSE
    dlclose(handle);
#endif
    handle = NULL;
    info = NULL;
  }
}

const TSS2_TCTI_INFO *tpm2_tcti_ldr_getinfo(void) {
  return info;
}

static void* tpm2_tcti_ldr_dlopen(const char *name) {
  char path[PATH_MAX];
  size_t size = snprintf(path, sizeof(path), TSS2_TCTI_SO_FORMAT, name);
  if (size >= sizeof(path)) {
    return NULL;
  }

  return dlopen(path, RTLD_LAZY);
}

bool tpm2_tcti_ldr_is_tcti_present(const char *name) {
  void *handle = tpm2_tcti_ldr_dlopen(name);
  if (handle) {
    dlclose(handle);
  }

  return handle != NULL;
}

TSS2_TCTI_CONTEXT *tpm2_tcti_ldr_load(const char *path) {
  TSS2_TCTI_CONTEXT *tcti_ctx = NULL;

  if (handle) {

    return NULL;
  }

  /*
  * Try what they gave us, if it doesn't load up, try
  * libtss2-tcti-xxx.so replacing xxx with what they gave us.
  */
  handle = dlopen (path, RTLD_LAZY);
  if (!handle) {

    handle = tpm2_tcti_ldr_dlopen(path);
    if (!handle) {
      ERROR_MSG("Could not dlopen library: \"%s\"", path);
      return NULL;
    }
  }

  TSS2_TCTI_INFO_FUNC infofn = (TSS2_TCTI_INFO_FUNC)dlsym(handle, TSS2_TCTI_INFO_SYMBOL);
  if (!infofn) {
    ERROR_MSG("Symbol \"%s\"not found in library: \"%s\"", TSS2_TCTI_INFO_SYMBOL, path);
    free(tcti_ctx);
  	dlclose(handle);
  	return NULL;
  }

  info = infofn();

  TSS2_TCTI_INIT_FUNC init = info->init;

  size_t size;
  TSS2_RC rc = init(NULL, &size, NULL);
  if (rc != TPM2_RC_SUCCESS) {
    ERROR_MSG("tcti init setup routine failed for library: \"%s\"", path);
    free(tcti_ctx);
  	dlclose(handle);
  	return NULL;
  }

  tcti_ctx = (TSS2_TCTI_CONTEXT*) calloc(1, size);
  if (tcti_ctx == NULL) {
    free(tcti_ctx);
  	dlclose(handle);
  	return NULL;
  }

  rc = init(tcti_ctx, &size, NULL);
  if (rc != TPM2_RC_SUCCESS) {
    ERROR_MSG("tcti init allocation routine failed for library: \"%s\"", path);
    free(tcti_ctx);
  	dlclose(handle);
  	return NULL;
  }

  return tcti_ctx;
}
#endif

// Mutex callback
void lock_callback(int mode, int n, const char* file, int line)
{
	if ((unsigned) n >= nlocks)
	{
		ERROR_MSG("out of range [0..%u[ lock %d at %s:%d",
			  nlocks, n, file, line);

		return;
	}

	Mutex* mtx = locks[(unsigned) n];

	if (mode & CRYPTO_LOCK)
	{
		mtx->lock();
	}
	else
	{
		mtx->unlock();
	}
}

// Constructor
OSSLCryptoFactory::OSSLCryptoFactory()
{
	// Multi-thread support
	nlocks = CRYPTO_num_locks();
	locks = new Mutex*[nlocks];
	for (unsigned i = 0; i < nlocks; i++)
	{
		locks[i] = MutexFactory::i()->getMutex();
	}

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	setLockingCallback = false;
	if (CRYPTO_get_locking_callback() == NULL)
	{
		CRYPTO_set_locking_callback(lock_callback);
		setLockingCallback = true;
	}
#endif

#ifdef WITH_FIPS
	// Already in FIPS mode on reenter (avoiding selftests)
	if (!FIPS_mode())
	{
		FipsSelfTestStatus = false;
		if (!FIPS_mode_set(1))
		{
			ERROR_MSG("can't enter into FIPS mode");
			return;
		}
	} else {
		// Undo RAND_cleanup()
		RAND_init_fips();
	}
	FipsSelfTestStatus = true;
#endif

	// Initialise OpenSSL
	OpenSSL_add_all_algorithms();

	// Make sure RDRAND is loaded first
	ENGINE_load_rdrand();
	// Locate the engine
	rdrand_engine = ENGINE_by_id("rdrand");
	// Use RDRAND if available
	if (rdrand_engine != NULL)
	{
		// Initialize RDRAND engine
		if (!ENGINE_init(rdrand_engine))
		{
			WARNING_MSG("ENGINE_init returned %lu\n", ERR_get_error());
		}
		// Set RDRAND engine as the default for RAND_ methods
		else if (!ENGINE_set_default(rdrand_engine, ENGINE_METHOD_RAND))
		{
			WARNING_MSG("ENGINE_set_default returned %lu\n", ERR_get_error());
		}
	}

	// Initialise the one-and-only RNG
	rng = new OSSLRNG();

#ifdef USE_TPM
	size_t size = 0;
  	TSS2_RC rc;

	tcti = tpm2_tcti_ldr_load("tabrmd");
  	if (!tcti)
  	{
    	ERROR_MSG("OSSLCryptoFactory: TPM2 Failed!"); 
    	return; 
  	}

  	size = Tss2_Sys_GetContextSize(0);
  	context = (TSS2_SYS_CONTEXT*) calloc(1, size);
	if (context == NULL)
	{
		ERROR_MSG("OSSLCryptoFactory: TPM2 Failed 2!");
		return;
	}

	TSS2_ABI_VERSION abi_version = TSS2_ABI_VERSION_CURRENT;
  
	rc = Tss2_Sys_Initialize(context, size, tcti, &abi_version);
	if (rc != TSS2_RC_SUCCESS)
	{
		ERROR_MSG("OSSLCryptoFactory: TPM2 Failed 3!");
		free(context);
		return;
	}
#endif

#ifdef WITH_GOST
	// Load engines
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	ENGINE_load_builtin_engines();
#else
	OPENSSL_init_crypto(OPENSSL_INIT_ENGINE_ALL_BUILTIN |
			    OPENSSL_INIT_ENGINE_RDRAND |
			    OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
			    OPENSSL_INIT_ADD_ALL_CIPHERS |
			    OPENSSL_INIT_ADD_ALL_DIGESTS |
			    OPENSSL_INIT_LOAD_CONFIG, NULL);
#endif

	// Initialise the GOST engine
	eg = ENGINE_by_id("gost");
	if (eg == NULL)
	{
		ERROR_MSG("can't get the GOST engine");
		return;
	}
	if (ENGINE_init(eg) <= 0)
	{
		ENGINE_free(eg);
		eg = NULL;
		ERROR_MSG("can't initialize the GOST engine");
		return;
	}
	// better than digest_gost
	EVP_GOST_34_11 = ENGINE_get_digest(eg, NID_id_GostR3411_94);
	if (EVP_GOST_34_11 == NULL)
	{
		ERROR_MSG("can't get the GOST digest");
		goto err;
	}
	// from the openssl.cnf
	if (ENGINE_register_pkey_asn1_meths(eg) <= 0)
	{
		ERROR_MSG("can't register ASN.1 for the GOST engine");
		goto err;
	}
	if (ENGINE_ctrl_cmd_string(eg,
				   "CRYPT_PARAMS",
				   "id-Gost28147-89-CryptoPro-A-ParamSet",
				   0) <= 0)
	{
		ERROR_MSG("can't set params of the GOST engine");
		goto err;
	}
	return;

err:
	ENGINE_finish(eg);
	ENGINE_free(eg);
	eg = NULL;
	return;
#endif
}

// Destructor
OSSLCryptoFactory::~OSSLCryptoFactory()
{
#ifdef USE_TPM
	TSS2_TCTI_CONTEXT *tcti_ctx;

	tcti_ctx = NULL;
	if (Tss2_Sys_GetTctiContext(context, &tcti_ctx) != TSS2_RC_SUCCESS) {
		tcti_ctx = NULL;
	}

	Tss2_Sys_Finalize(context);
  	free(context);

	if (tcti_ctx) {
		Tss2_Tcti_Finalize(tcti_ctx);
		free(tcti_ctx);
		tcti_ctx = NULL;
	}

	tpm2_tcti_ldr_unload();
#endif

#ifdef WITH_GOST
	// Finish the GOST engine
	if (eg != NULL)
	{
		ENGINE_finish(eg);
		ENGINE_free(eg);
		eg = NULL;
	}
#endif

	// Destroy the one-and-only RNG
	delete rng;

	// Recycle locks
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	if (setLockingCallback)
	{
		CRYPTO_set_locking_callback(NULL);
	}
#endif
	for (unsigned i = 0; i < nlocks; i++)
	{
		MutexFactory::i()->recycleMutex(locks[i]);
	}
	delete[] locks;
}

// Return the one-and-only instance
OSSLCryptoFactory* OSSLCryptoFactory::i()
{
	if (!instance.get())
	{
		instance.reset(new OSSLCryptoFactory());
	}

	return instance.get();
}

// This will destroy the one-and-only instance.
void OSSLCryptoFactory::reset()
{
	instance.reset();
}

#ifdef WITH_FIPS
bool OSSLCryptoFactory::getFipsSelfTestStatus() const
{
	return FipsSelfTestStatus;
}
#endif

// Create a concrete instance of a symmetric algorithm
SymmetricAlgorithm* OSSLCryptoFactory::getSymmetricAlgorithm(SymAlgo::Type algorithm)
{
	switch (algorithm)
	{
		case SymAlgo::AES:
			return new OSSLAES();
		case SymAlgo::DES:
		case SymAlgo::DES3:
			return new OSSLDES();
		default:
			// No algorithm implementation is available
			ERROR_MSG("Unknown algorithm '%i'", algorithm);

			return NULL;
	}

	// No algorithm implementation is available
	return NULL;
}

// Create a concrete instance of an asymmetric algorithm
AsymmetricAlgorithm* OSSLCryptoFactory::getAsymmetricAlgorithm(AsymAlgo::Type algorithm)
{
	switch (algorithm)
	{
		case AsymAlgo::RSA:
			return new OSSLRSA();
		case AsymAlgo::DSA:
			return new OSSLDSA();
		case AsymAlgo::DH:
			return new OSSLDH();
#ifdef WITH_ECC
		case AsymAlgo::ECDH:
			return new OSSLECDH();
		case AsymAlgo::ECDSA:
			return new OSSLECDSA();
#endif
#ifdef WITH_GOST
		case AsymAlgo::GOST:
			return new OSSLGOST();
#endif
#ifdef WITH_EDDSA
		case AsymAlgo::EDDSA:
			return new OSSLEDDSA();
#endif
		default:
			// No algorithm implementation is available
			ERROR_MSG("Unknown algorithm '%i'", algorithm);

			return NULL;
	}

	// No algorithm implementation is available
	return NULL;
}

// Create a concrete instance of a hash algorithm
HashAlgorithm* OSSLCryptoFactory::getHashAlgorithm(HashAlgo::Type algorithm)
{
	switch (algorithm)
	{
		case HashAlgo::MD5:
			return new OSSLMD5();
		case HashAlgo::SHA1:
			return new OSSLSHA1();
		case HashAlgo::SHA224:
			return new OSSLSHA224();
		case HashAlgo::SHA256:
			return new OSSLSHA256();
		case HashAlgo::SHA384:
			return new OSSLSHA384();
		case HashAlgo::SHA512:
			return new OSSLSHA512();
#ifdef WITH_GOST
		case HashAlgo::GOST:
			return new OSSLGOSTR3411();
#endif
		default:
			// No algorithm implementation is available
			ERROR_MSG("Unknown algorithm '%i'", algorithm);

			return NULL;
	}

	// No algorithm implementation is available
	return NULL;
}

// Create a concrete instance of a MAC algorithm
MacAlgorithm* OSSLCryptoFactory::getMacAlgorithm(MacAlgo::Type algorithm)
{
	switch (algorithm)
	{
		case MacAlgo::HMAC_MD5:
			return new OSSLHMACMD5();
		case MacAlgo::HMAC_SHA1:
			return new OSSLHMACSHA1();
		case MacAlgo::HMAC_SHA224:
			return new OSSLHMACSHA224();
		case MacAlgo::HMAC_SHA256:
			return new OSSLHMACSHA256();
		case MacAlgo::HMAC_SHA384:
			return new OSSLHMACSHA384();
		case MacAlgo::HMAC_SHA512:
			return new OSSLHMACSHA512();
#ifdef WITH_GOST
		case MacAlgo::HMAC_GOST:
			return new OSSLHMACGOSTR3411();
#endif
		case MacAlgo::CMAC_DES:
			return new OSSLCMACDES();
		case MacAlgo::CMAC_AES:
			return new OSSLCMACAES();
		default:
			// No algorithm implementation is available
			ERROR_MSG("Unknown algorithm '%i'", algorithm);

			return NULL;
	}

	// No algorithm implementation is available
	return NULL;
}

// Get the global RNG (may be an unique RNG per thread)
RNG* OSSLCryptoFactory::getRNG(RNGImpl::Type name /* = RNGImpl::Default */)
{
	if (name == RNGImpl::Default)
	{
		return rng;
	}
	else
	{
		// No RNG implementation is available
		ERROR_MSG("Unknown RNG '%i'", name);

		return NULL;
	}
}

