/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2010-2016 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_CRYPTO_H_
#define _PASSENGER_CRYPTO_H_

#include <boost/function.hpp>
#include <boost/thread.hpp>
#include <modp_b64.h>

#if BOOST_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#endif

#include <StaticString.h>

namespace Passenger {

#if BOOST_OS_MACOS
typedef SecKeyRef PUBKEY_TYPE;
#else
typedef RSA* PUBKEY_TYPE;
#endif

using namespace std;
using namespace boost;
using namespace oxt;

struct AESEncResult {
	unsigned char *encrypted;
	size_t encryptedLen;
	unsigned char *key;
	size_t keyLen;
	unsigned char *iv;
	size_t ivLen;
};

class Crypto {
private:
	/**
	 * @returns new PUBKEY_TYPE; user is responsible for free (with freePubKey)
	 */
	PUBKEY_TYPE loadPubKey(const char *filename);

	/**
	 * free a PUBKEY_TYPE (loaded with loadPubKey); may be NULL
	 */
	void freePubKey(PUBKEY_TYPE);

	void logError(const StaticString &error);

	/**
	 * log prefix using P_ERROR, and (library-specific) detail from either additional or global query
	 */
#if BOOST_OS_MACOS
	// (additional needs to be defined as a CFErrorRef, void * won't work)
	void logFreeErrorExtended(const StaticString &prefix, CFErrorRef &additional);
	CFDictionaryRef createQueryDict(const char *label);
	SecAccessRef createAccess(const char *cLabel);
	OSStatus lookupKeychainItem(const char *label, SecIdentityRef *oIdentity);
	OSStatus copyIdentityFromPKCS12File(const char *cPath, const char *cPassword, const char *cLabel);
	CFDataRef genIV(size_t iv_size);
	bool getKeyBytes(SecKeyRef cryptokey, void **target, size_t &len);
	bool memoryBridge(CFDataRef input, void **target, size_t &len);
	bool innerMemoryBridge(void *input, void **target, size_t len);
#else
	void logErrorExtended(const StaticString &prefix);
#endif

public:
	Crypto();
	~Crypto();

	/**
	 * Generates a nonce consisting of a timestamp (usec) and a random (base64) part.
	 */
	bool generateAndAppendNonce(string &nonce);

#if BOOST_OS_MACOS
	/**
	 * sets the permissions on the certificate so that curl doesn't prompt
	 */
	bool preAuthKey(const char *path, const char *passwd, const char *cLabel);
	void killKey(const char *cLabel);
	bool generateRandomChars(unsigned char *rndChars, int rndLen);
#endif

	/**
	 * Generate an AES key and encrypt dataChars with it. The resulting encrypted characters, together with the AES key
	 * and iv that were used appears in AESEncrypted. Memory is allocated for it, and it must be freed with freeAESEncrypted().
	 *
	 * N.B. only used in Enterprise (to enable additional services), but open sourced for transparency.
	 */
	bool encryptAES256(char *dataChars, size_t dataLen, AESEncResult &aesEnc);

	/**
	 * Releases resources returned by encryptAES256().
	 */
	void freeAESEncrypted(AESEncResult &aesEnc);

	/**
	 * Encrypt a (short) bit of date with specified public key. Memory is allocated for the result encryptedCharsPtr, which
	 * must be free()d manually.
	 *
	 * N.B. only used in Enterprise (to enable additional services), but open sourced for transparency.
	 */
	bool encryptRSA(unsigned char *dataChars, size_t dataLen,
			string encryptPubKeyPath, unsigned char **encryptedCharsPtr, size_t &encryptedLen);

	/**
	 * @returns true if specified signature is from the entity known by its (public) key at signaturePubKeyPath,
	 * and valid for speficied data.
	 */
	bool verifySignature(string signaturePubKeyPath, char *signatureChars, int signatureLen, string data);

};

} // namespace Passenger

#endif /* _PASSENGER_CRYPTO_H_ */
