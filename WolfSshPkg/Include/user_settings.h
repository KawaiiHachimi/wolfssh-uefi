#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

/* Platform: single-threaded UEFI application, no POSIX layer. */
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_WRITEV
#define WOLFSSL_NO_SOCK
#define WOLFSSL_IGNORE_FILE_WARN
#define WOLFSSL_GENERAL_ALIGNMENT 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_LONG 8
#define NO_DEV_RANDOM
#define NO_ASN_TIME
#define NO_WOLFSSL_DIR
#define NO_OLD_RNGNAME
#define WOLFSSL_NO_ASM
#define HAVE___UINT128_T

/* wolfSSH client and its user-supplied EDK II TCP callbacks. */
#define WOLFSSL_WOLFSSH
#define WOLFCRYPT_ONLY
#define NO_WOLFSSH_SERVER
#define WOLFSSH_USER_IO
#define WOLFSSH_TERM
#define WOLFSSH_UEFI
#define NO_TERMIOS
#define WOLFSSH_NO_DEFAULT_LOGGING_CB
#define WOLFSSH_NO_TIMESTAMP

/* Modern, compact interoperability profile. */
#define HAVE_ECC
#define ECC_USER_CURVES
#define ECC_TIMING_RESISTANT
#define HAVE_WC_ECC_SET_RNG
#define WOLFSSL_PUBLIC_MP
#define WOLFSSL_SP_MATH
#define WOLFSSL_SP_SMALL
#define WOLFSSL_HAVE_SP_ECC
#undef NO_ECC256

#define HAVE_AESGCM
#define GCM_SMALL
#define WOLFSSL_AES_COUNTER
#define WOLFSSL_AES_SMALL_TABLES
#define WOLFSSL_AES_NO_UNROLL
#define NO_AES_CBC

/* SHA-256 and HMAC-SHA-256 remain enabled. */
#define NO_SHA
#define NO_SHA512

/* UEFI RNG protocol supplies cryptographically strong bytes directly. */
#define WC_NO_HASHDRBG
extern int UefiRandomGenerateBlock(unsigned char* output, unsigned int size);
#define CUSTOM_RAND_GENERATE_BLOCK UefiRandomGenerateBlock

/* UEFI pool allocator. The heap/type arguments remain part of wolfSSL ABI. */
#define XMALLOC_OVERRIDE
extern void* UefiWolfAllocate(unsigned long size);
extern void UefiWolfFree(void* buffer);
extern void* UefiWolfReallocate(void* buffer, unsigned long size);
#define XMALLOC(size, heap, type) \
    ((void)(heap), (void)(type), UefiWolfAllocate((unsigned long)(size)))
#define XFREE(buffer, heap, type) \
    ((void)(heap), (void)(type), UefiWolfFree((buffer)))
#define XREALLOC(buffer, size, heap, type) \
    ((void)(heap), (void)(type), \
     UefiWolfReallocate((buffer), (unsigned long)(size)))

/* Algorithms and features not required by this first client profile. */
#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_DES3
#define NO_RC4
#define NO_MD4
#define NO_MD5
#define NO_PSK
#define NO_PWDBASED
#define NO_PKCS8
#define NO_PKCS12
#define NO_CERTS
#define NO_SESSION_CACHE
#define NO_ERROR_STRINGS
#define WC_NO_ASYNC_THREADING
#define WOLFSSL_NO_TLS12
#define NO_OLD_TLS

#endif /* WOLFSSL_USER_SETTINGS_H */
