// Small Linux AES-GCM bridge for the isolated test proxy, avoiding a Python crypto dependency.
#include <openssl/evp.h>

int udp_test_gcm(int encrypt, unsigned char *key, unsigned char *nonce, unsigned char *aad,
                 unsigned char *data, int size, unsigned char *output, unsigned char *tag) {
  if ((encrypt != 0 && encrypt != 1) || size < 0 || size > 1200)
    return -1;
  EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
  if (!context)
    return -1;
  int written = 0, tail = 0;
  int ok = EVP_CipherInit_ex(context, EVP_aes_256_gcm(), NULL, key, nonce, encrypt) &&
           EVP_CipherUpdate(context, NULL, &written, aad, 20) &&
           EVP_CipherUpdate(context, output, &written, data, size);
  if (!encrypt)
    ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, 16, tag);
  ok = ok && EVP_CipherFinal_ex(context, output + written, &tail);
  if (encrypt)
    ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, 16, tag);
  EVP_CIPHER_CTX_free(context);
  return ok ? written + tail : -1;
}
