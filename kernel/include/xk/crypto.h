// Cryptography exports (spec/stock/15): SHA-1, HMAC, RC4, DES and the public-key calls.
#pragma once
#include <xk/types.h>

// Hashes and RC4 (crypto/public_key.cpp, through the crypto vector below).
XBAPI void NTAPI XcSHAInit(PUCHAR pbSHAContext);
XBAPI void NTAPI XcSHAUpdate(PUCHAR pbSHAContext, const UCHAR *pbInput, ULONG dwInputLength);
XBAPI void NTAPI XcSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest);
XBAPI void NTAPI XcHMAC(const UCHAR *pbKeyMaterial, ULONG cbKeyMaterial, const UCHAR *pbData, ULONG cbData,
                        const UCHAR *pbData2, ULONG cbData2, PUCHAR HmacData);
XBAPI void NTAPI XcRC4Key(PUCHAR pbKeyStruct, ULONG dwKeyLength, const UCHAR *pbKey);
XBAPI void NTAPI XcRC4Crypt(PUCHAR pbKeyStruct, ULONG dwInputLength, PUCHAR pbInput);

// Public-key operations (RSA keys in the BSAFE format).
XBAPI ULONG NTAPI XcPKEncPublic(PUCHAR pbPubKey, PUCHAR pbInput, PUCHAR pbOutput);
XBAPI ULONG NTAPI XcPKDecPrivate(PUCHAR pbPrvKey, PUCHAR pbInput, PUCHAR pbOutput);
XBAPI ULONG NTAPI XcPKGetKeyLen(PUCHAR pbPubKey);
XBAPI BOOLEAN NTAPI XcVerifyPKCS1Signature(PUCHAR pbSig, PUCHAR pbPubKey, PUCHAR pbDigest);
XBAPI ULONG NTAPI XcModExp(PULONG pA, PULONG pB, PULONG pC, PULONG pD, ULONG dwN);

// DES and triple DES, and the service entry that dispatches to all of these.
XBAPI void NTAPI XcDESKeyParity(PUCHAR pbKey, ULONG dwKeyLength);
XBAPI void NTAPI XcKeyTable(ULONG dwCipher, PUCHAR pbKeyTable, PUCHAR pbKey);
XBAPI void NTAPI XcBlockCrypt(ULONG dwCipher, PUCHAR pbOutput, PUCHAR pbInput, PUCHAR pbKeyTable, ULONG dwOp);
XBAPI void NTAPI XcBlockCryptCBC(ULONG dwCipher, ULONG dwInputLength, PUCHAR pbOutput, PUCHAR pbInput,
                                 PUCHAR pbKeyTable, ULONG dwOp, PUCHAR pbFeedback);
XBAPI ULONG NTAPI XcCryptService(ULONG dwOp, PVOID pArgs);

// The implementations behind the exports above (crypto/crypto.cpp), which XcUpdateCrypto can replace.
XBAPI void NTAPI XcpSHAInit(PUCHAR pbSHAContext);
XBAPI void NTAPI XcpSHAUpdate(PUCHAR pbSHAContext, const UCHAR *pbInput, ULONG dwInputLength);
XBAPI void NTAPI XcpSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest);
XBAPI void NTAPI XcpRC4Key(PUCHAR pbKeyStruct, ULONG dwKeyLength, const UCHAR *pbKey);
XBAPI void NTAPI XcpRC4Crypt(PUCHAR pbKeyStruct, ULONG dwInputLength, PUCHAR pbInput);
XBAPI void NTAPI XcpHMAC(const UCHAR *pbKeyMaterial, ULONG cbKeyMaterial, const UCHAR *pbData, ULONG cbData,
                        const UCHAR *pbData2, ULONG cbData2, PUCHAR HmacData);

// The crypto vector: the exports above call through it; XcUpdateCrypto replaces its non-null entries.
struct CRYPTO_VECTOR {
    void(NTAPI *SHAInit)(PUCHAR);
    void(NTAPI *SHAUpdate)(PUCHAR, const UCHAR *, ULONG);
    void(NTAPI *SHAFinal)(PUCHAR, PUCHAR);
    void(NTAPI *RC4Key)(PUCHAR, ULONG, const UCHAR *);
    void(NTAPI *RC4Crypt)(PUCHAR, ULONG, PUCHAR);
    void(NTAPI *HMAC)(const UCHAR *, ULONG, const UCHAR *, ULONG, const UCHAR *, ULONG, PUCHAR);
    ULONG(NTAPI *PKEncPublic)(PUCHAR, PUCHAR, PUCHAR);
    ULONG(NTAPI *PKDecPrivate)(PUCHAR, PUCHAR, PUCHAR);
    ULONG(NTAPI *PKGetKeyLen)(PUCHAR);
    BOOLEAN(NTAPI *VerifyPKCS1Signature)(PUCHAR, PUCHAR, PUCHAR);
    ULONG(NTAPI *ModExp)(PULONG, PULONG, PULONG, PULONG, ULONG);
    void(NTAPI *DESKeyParity)(PUCHAR, ULONG);
    void(NTAPI *KeyTable)(ULONG, PUCHAR, PUCHAR);
    void(NTAPI *BlockCrypt)(ULONG, PUCHAR, PUCHAR, PUCHAR, ULONG);
    void(NTAPI *BlockCryptCBC)(ULONG, ULONG, PUCHAR, PUCHAR, PUCHAR, ULONG, PUCHAR);
    ULONG(NTAPI *CryptService)(ULONG, PVOID);
};
static_assert(sizeof(CRYPTO_VECTOR) == 64);
XBAPI void NTAPI XcUpdateCrypto(CRYPTO_VECTOR *pNewVector, PUCHAR pROMHash);
