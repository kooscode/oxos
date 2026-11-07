# Spec 15 — Xc crypto services

Evidence: objdump of the export stubs (0x8002f937–0x8002f9fb) and XcUpdateCrypto (0x8002f84c), the default vector at
0x80036270, and batch_decompile `ev_9eece4e7…`, `ev_d5297719…`, `ev_f406717f…` (RSA encrypt/decrypt, key-length,
PKCS#1 check, modexp, DES parity, key table, block crypt, CBC, crypt service). The algorithms are the published
SHA-1, HMAC, RC4, RSA and DES (FIPS 46-3). **Status: implemented (milestone 5c).**

## Crypto vector
- The 16 exported Xc functions go through a function table (SHAInit, SHAUpdate, SHAFinal, RC4Key, RC4Crypt, HMAC,
  PKEncPublic, PKDecPrivate, PKGetKeyLen, VerifyPKCS1Signature, ModExp, DESKeyParity, KeyTable, BlockCrypt,
  BlockCryptCBC, CryptService).
- `XcUpdateCrypto(vector, romHash)` replaces every non-NULL entry. The reference copies a 64-byte table out to
  romHash; ours returns zeros.
- XcCryptService returns 0.

## Public key (BSAFE key blobs)
- **'RSA1' public:** {magic 0x31415352, KeyLength (modulus buffer bytes), BitLength, DataLength (= bits/8 − 1),
  PublicExponent, modulus[KeyLength]}.
- **'RSA2' private:** the same header, then modulus[KeyLength], prime1, prime2, exponent1, exponent2, coefficient
  (KeyLength/2 each), private exponent[KeyLength].
- Numbers are little-endian. Operations use N = 2 × (bits/64 + (bits % 64 ≠ 0 after /2 ? 2 : 1)) dwords.
- XcPKGetKeyLen returns KeyLength.
- **XcPKEncPublic(key, in, out):** exponent 1 copies; the input must be below the modulus; out = in^e mod n; 1/0.
  XcPKDecPrivate does the same with the private key (the reference uses CRT).
- **XcVerifyPKCS1Signature(sig, pubKey, digest):** decrypt the signature (BitLength bits), then check the
  little-endian block: digest reversed (20 bytes), an optional DigestInfo prefix, a 00 separator, FF padding,
  01 at DataLength−1 and 00 at DataLength.
- **XcModExp(A, B, C, D, N):** A = B^C mod D over N dwords.

## DES
- XcDESKeyParity forces odd parity on each byte.
- XcKeyTable(cipher, table, key): cipher 0 = DES (128-byte table), 1 = 3DES (three tables, 24-byte key).
- XcBlockCrypt(cipher, out, in, table, op): op 1 encrypts, 0 decrypts; 3DES is EDE.
- XcBlockCryptCBC(cipher, length, out, in, table, op, feedback) chains 8-byte blocks through the feedback register.
