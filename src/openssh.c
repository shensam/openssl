#include <Rinternals.h>
#include <string.h>
#include "apple.h"
#include "utils.h"
#include <openssl/pem.h>
#include <openssl/bn.h>

/* BN_num_bytes() drops leading zeros which can alter openssh fingerprint */
SEXP bignum_to_r_size(BIGNUM *bn, int bytes){
  int bits = BN_num_bits(bn);
  if(bytes == 0)
    bytes = (bits/8) + 1;
  int numbytes = BN_num_bytes(bn);
  int diff = bytes - numbytes;
  SEXP res = allocVector(RAWSXP, bytes);
  unsigned char *ptr = RAW(res);
  memset(ptr, 0, diff);
  ptr += diff;
  BN_bn2bin(bn, ptr);
  return res;
}

SEXP bignum_to_r(BIGNUM *bn){
  return bignum_to_r_size(bn, 0);
}

/* Manuall compose public keys from bignum values */
SEXP R_rsa_build(SEXP expdata, SEXP moddata){
  RSA *rsa = RSA_new();
  rsa->e = BN_new();
  rsa->n = BN_new();
  bail(!!BN_bin2bn(RAW(expdata), LENGTH(expdata), rsa->e));
  bail(!!BN_bin2bn(RAW(moddata), LENGTH(moddata), rsa->n));
  unsigned char *buf = NULL;
  int len = i2d_RSA_PUBKEY(rsa, &buf);
  bail(len);
  SEXP res = allocVector(RAWSXP, len);
  memcpy(RAW(res), buf, len);
  free(buf);
  return res;
}

SEXP R_rsa_decompose(SEXP bin){
  RSA *rsa = RSA_new();
  const unsigned char *ptr = RAW(bin);
  bail(!!d2i_RSA_PUBKEY(&rsa, &ptr, LENGTH(bin)));
  SEXP res = PROTECT(allocVector(VECSXP, 2));
  SET_VECTOR_ELT(res, 0, bignum_to_r(rsa->e));
  SET_VECTOR_ELT(res, 1, bignum_to_r(rsa->n));
  UNPROTECT(1);
  return res;
}

// See https://tools.ietf.org/html/rfc4253: ... the "ssh-dss" key format has ...
SEXP R_dsa_build(SEXP p, SEXP q, SEXP g, SEXP y){
  DSA *dsa = DSA_new();
  dsa->p = BN_new();
  dsa->q = BN_new();
  dsa->g = BN_new();
  dsa->pub_key = BN_new();
  bail(!!BN_bin2bn(RAW(p), LENGTH(p), dsa->p));
  bail(!!BN_bin2bn(RAW(q), LENGTH(q), dsa->q));
  bail(!!BN_bin2bn(RAW(g), LENGTH(g), dsa->g));
  bail(!!BN_bin2bn(RAW(y), LENGTH(y), dsa->pub_key));
  unsigned char *buf = NULL;
  int len = i2d_DSA_PUBKEY(dsa, &buf);
  bail(len);
  SEXP res = allocVector(RAWSXP, len);
  memcpy(RAW(res), buf, len);
  free(buf);
  return res;
}

SEXP R_dsa_decompose(SEXP bin){
  DSA *dsa = DSA_new();
  const unsigned char *ptr = RAW(bin);
  bail(!!d2i_DSA_PUBKEY(&dsa, &ptr, LENGTH(bin)));
  SEXP res = PROTECT(allocVector(VECSXP, 4));
  SET_VECTOR_ELT(res, 0, bignum_to_r(dsa->p));
  SET_VECTOR_ELT(res, 1, bignum_to_r(dsa->q));
  SET_VECTOR_ELT(res, 2, bignum_to_r(dsa->g));
  SET_VECTOR_ELT(res, 3, bignum_to_r(dsa->pub_key));
  UNPROTECT(1);
  return res;
}

// EC_curve_nist2nid only available in recent openssl versions
int my_nist2nid(const char *name){
  if (!strcmp(name, "P-256")){
    return NID_X9_62_prime256v1;
  } else if (!strcmp(name, "P-384")){
    return NID_secp384r1;
  } else if (!strcmp(name, "P-521")){
    return NID_secp521r1;
  }
  return 0;
}

const char *my_nid2nist(int nid){
  switch(nid){
    case NID_X9_62_prime256v1:
      return "P-256";
    case NID_secp384r1:
      return "P-384";
    case NID_secp521r1:
      return "P-521";
  }
  return "";
}

int nid_keysize(int nid){
  switch(nid){
  case NID_X9_62_prime256v1:
    return 32;
  case NID_secp384r1:
    return 48;
  case NID_secp521r1:
    return 66;
  }
  return 0;
}

SEXP R_ecdsa_build(SEXP x, SEXP y, SEXP nist){
  int nid = my_nist2nid(CHAR(STRING_ELT(nist, 0)));
  bail(nid);
  EC_KEY *pubkey = EC_KEY_new_by_curve_name(nid);
  EC_KEY_set_asn1_flag(pubkey, OPENSSL_EC_NAMED_CURVE);
  if(!EC_KEY_set_public_key_affine_coordinates(pubkey, BN_bin2bn(RAW(x), LENGTH(x), NULL), BN_bin2bn(RAW(y), LENGTH(y), NULL)))
    error("Failed to construct EC key. Perhaps invalid point or curve.");
  unsigned char *buf = NULL;
  int len = i2d_EC_PUBKEY(pubkey, &buf);
  bail(len);
  SEXP res = allocVector(RAWSXP, len);
  memcpy(RAW(res), buf, len);
  free(buf);
  return res;
}

SEXP R_ecdsa_decompose(SEXP input){
  const unsigned char *ptr = RAW(input);
  EVP_PKEY *pkey = d2i_PUBKEY(NULL, &ptr, LENGTH(input));
  bail(!!pkey);
  EC_KEY *ec = EVP_PKEY_get1_EC_KEY(pkey);
  const EC_POINT *pubkey = EC_KEY_get0_public_key(ec);
  const EC_GROUP *group = EC_KEY_get0_group(ec);
  int nid = EC_GROUP_get_curve_name(group);
  int keysize = nid_keysize(nid);
  BIGNUM *x = BN_new();
  BIGNUM *y = BN_new();
  BN_CTX *ctx = BN_CTX_new();
  bail(EC_POINT_get_affine_coordinates_GFp(group, pubkey, x, y, ctx));
  BN_CTX_free(ctx);
  SEXP res = PROTECT(allocVector(VECSXP, 3));
  SET_VECTOR_ELT(res, 0, mkString(my_nid2nist(nid)));
  SET_VECTOR_ELT(res, 1, bignum_to_r_size(x, keysize));
  SET_VECTOR_ELT(res, 2, bignum_to_r_size(y, keysize));
  BN_free(x);
  BN_free(y);
  UNPROTECT(1);
  return res;
}