/*
 *  Privacy Enhanced Mail (PEM) decoding
 *
 *  Copyright (C) 2006-2014, ARM Limited, All Rights Reserved
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "net.h"
#if !defined(MBEDTLS_CONFIG_FILE)
#include "config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_PEM_PARSE_C) || defined(MBEDTLS_PEM_WRITE_C)

#include "pem.h"
#include "base64.h"
#include "des.h"
#include "aes.h"
#include "md5.h"
#include "cipher.h"

#include <string.h>

#if defined(MBEDTLS_PLATFORM_C)
#include "platform.h"
#else
#include <stdlib.h>
#define mbedtls_calloc    calloc
#define mbedtls_free       free
#endif

/* Implementation that should never be optimized out by the compiler */
static void mbedtls_zeroize( void *v, size_t n ) {
    volatile unsigned char *p = v; while( n-- ) *p++ = 0;
}

#if defined(MBEDTLS_PEM_PARSE_C)
void mbedtls_pem_init( mbedtls_pem_context *ctx )
{
    memset( ctx, 0, sizeof( mbedtls_pem_context ) );
}

#if defined(MBEDTLS_MD5_C) && defined(MBEDTLS_CIPHER_MODE_CBC) &&         \
    ( defined(MBEDTLS_DES_C) || defined(MBEDTLS_AES_C) )
/*
 * Read a 16-byte hex string and convert it to binary
 */
static int pem_get_iv( const unsigned char *s, unsigned char *iv,
                       size_t iv_len )
{
    size_t i, j, k;

    memset( iv, 0, iv_len );

    for( i = 0; i < iv_len * 2; i++, s++ )
    {
        if( *s >= '0' && *s <= '9' ) j = *s - '0'; else
        if( *s >= 'A' && *s <= 'F' ) j = *s - '7'; else
        if( *s >= 'a' && *s <= 'f' ) j = *s - 'W'; else
            return( MBEDTLS_ERR_PEM_INVALID_ENC_IV );

        k = ( ( i & 1 ) != 0 ) ? j : j << 4;

        iv[i >> 1] = (unsigned char)( iv[i >> 1] | k );
    }

    return( 0 );
}

static void pem_pbkdf1( unsigned char *key, size_t keylen,
                        unsigned char *iv,
                        const unsigned char *pwd, size_t pwdlen )
{
    mbedtls_md5_context md5_ctx;
    unsigned char md5sum[16];
    size_t use_len;

    mbedtls_md5_init( &md5_ctx );

    /*
     * key[ 0..15] = MD5(pwd || IV)
     */
    mbedtls_md5_starts( &md5_ctx );
    mbedtls_md5_update( &md5_ctx, pwd, pwdlen );
    mbedtls_md5_update( &md5_ctx, iv,  8 );
    mbedtls_md5_finish( &md5_ctx, md5sum );

    if( keylen <= 16 )
    {
        memcpy( key, md5sum, keylen );

        mbedtls_md5_free( &md5_ctx );
        mbedtls_zeroize( md5sum, 16 );
        return;
    }

    memcpy( key, md5sum, 16 );

    /*
     * key[16..23] = MD5(key[ 0..15] || pwd || IV])
     */
    mbedtls_md5_starts( &md5_ctx );
    mbedtls_md5_update( &md5_ctx, md5sum,  16 );
    mbedtls_md5_update( &md5_ctx, pwd, pwdlen );
    mbedtls_md5_update( &md5_ctx, iv,  8 );
    mbedtls_md5_finish( &md5_ctx, md5sum );

    use_len = 16;
    if( keylen < 32 )
        use_len = keylen - 16;

    memcpy( key + 16, md5sum, use_len );

    mbedtls_md5_free( &md5_ctx );
    mbedtls_zeroize( md5sum, 16 );
}

#if defined(MBEDTLS_DES_C)
/*
 * Decrypt with DES-CBC, using PBKDF1 for key derivation
 */
static void pem_des_decrypt( unsigned char des_iv[8],
                               unsigned char *buf, size_t buflen,
                               const unsigned char *pwd, size_t pwdlen )
{
    mbedtls_des_context des_ctx;
    unsigned char des_key[8];

    mbedtls_des_init( &des_ctx );

    pem_pbkdf1( des_key, 8, des_iv, pwd, pwdlen );

    mbedtls_des_setkey_dec( &des_ctx, des_key );
    mbedtls_des_crypt_cbc( &des_ctx, MBEDTLS_DES_DECRYPT, buflen,
                     des_iv, buf, buf );

    mbedtls_des_free( &des_ctx );
    mbedtls_zeroize( des_key, 8 );
}

/*
 * Decrypt with 3DES-CBC, using PBKDF1 for key derivation
 */
static void pem_des3_decrypt( unsigned char des3_iv[8],
                               unsigned char *buf, size_t buflen,
                               const unsigned char *pwd, size_t pwdlen )
{
    mbedtls_des3_context des3_ctx;
    unsigned char des3_key[24];

    mbedtls_des3_init( &des3_ctx );

    pem_pbkdf1( des3_key, 24, des3_iv, pwd, pwdlen );

    mbedtls_des3_set3key_dec( &des3_ctx, des3_key );
    mbedtls_des3_crypt_cbc( &des3_ctx, MBEDTLS_DES_DECRYPT, buflen,
                     des3_iv, buf, buf );

    mbedtls_des3_free( &des3_ctx );
    mbedtls_zeroize( des3_key, 24 );
}
#endif /* MBEDTLS_DES_C */

#if defined(MBEDTLS_AES_C)
/*
 * Decrypt with AES-XXX-CBC, using PBKDF1 for key derivation
 */
static void pem_aes_decrypt( unsigned char aes_iv[16], unsigned int keylen,
                               unsigned char *buf, size_t buflen,
                               const unsigned char *pwd, size_t pwdlen )
{
    mbedtls_aes_context aes_ctx;
    unsigned char aes_key[32];

    mbedtls_aes_init( &aes_ctx );

    pem_pbkdf1( aes_key, keylen, aes_iv, pwd, pwdlen );

    mbedtls_aes_setkey_dec( &aes_ctx, aes_key, keylen * 8 );
    mbedtls_aes_crypt_cbc( &aes_ctx, MBEDTLS_AES_DECRYPT, buflen,
                     aes_iv, buf, buf );

    mbedtls_aes_free( &aes_ctx );
    mbedtls_zeroize( aes_key, keylen );
}
#endif /* MBEDTLS_AES_C */

#endif /* MBEDTLS_MD5_C && MBEDTLS_CIPHER_MODE_CBC &&
          ( MBEDTLS_AES_C || MBEDTLS_DES_C ) */

int mbedtls_pem_read_buffer( mbedtls_pem_context *ctx, const char *header, const char *footer,
                     const unsigned char *data, const unsigned char *pwd,
                     size_t pwdlen, size_t *use_len )
{
    int ret, enc;
    size_t len;
    unsigned char *buf;
    const unsigned char *s1, *s2, *end;
#if defined(MBEDTLS_MD5_C) && defined(MBEDTLS_CIPHER_MODE_CBC) &&         \
    ( defined(MBEDTLS_DES_C) || defined(MBEDTLS_AES_C) )
    unsigned char pem_iv[16];
    mbedtls_cipher_type_t enc_alg = MBEDTLS_CIPHER_NONE;
#else
    ((void) pwd);
    ((void) pwdlen);
#endif /* MBEDTLS_MD5_C && MBEDTLS_CIPHER_MODE_CBC &&
          ( MBEDTLS_AES_C || MBEDTLS_DES_C ) */

    if( ctx == NULL )
        return( MBEDTLS_ERR_PEM_BAD_INPUT_DATA );

    s1 = (unsigned char *) strstr( (const char *) data, header );

    if( s1 == NULL )
        return( MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT );

    s2 = (unsigned char *) strstr( (const char *) data, footer );

    if( s2 == NULL || s2 <= s1 )
        return( MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT );

    s1 += strlen( header );
    if( *s1 == '\r' ) s1++;
    if( *s1 == '\n' ) s1++;
    else return( MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT );

    end = s2;
    end += strlen( footer );
    if( *end == '\r' ) end++;
    if( *end == '\n' ) end++;
    *use_len = end - data;

    enc = 0;

    if( memcmp( s1, "Proc-Type: 4,ENCRYPTED", 22 ) == 0 )
    {
#if defined(MBEDTLS_MD5_C) && defined(MBEDTLS_CIPHER_MODE_CBC) &&         \
    ( defined(MBEDTLS_DES_C) || defined(MBEDTLS_AES_C) )
        enc++;

        s1 += 22;
        if( *s1 == '\r' ) s1++;
        if( *s1 == '\n' ) s1++;
        else return( MBEDTLS_ERR_PEM_INVALID_DATA );


#if defined(MBEDTLS_DES_C)
        if( memcmp( s1, "DEK-Info: DES-EDE3-CBC,", 23 ) == 0 )
        {
            enc_alg = MBEDTLS_CIPHER_DES_EDE3_CBC;

            s1 += 23;
            if( pem_get_iv( s1, pem_iv, 8 ) != 0 )
                return( MBEDTLS_ERR_PEM_INVALID_ENC_IV );

            s1 += 16;
        }
        else if( memcmp( s1, "DEK-Info: DES-CBC,", 18 ) == 0 )
        {
            enc_alg = MBEDTLS_CIPHER_DES_CBC;

            s1 += 18;
            if( pem_get_iv( s1, pem_iv, 8) != 0 )
                return( MBEDTLS_ERR_PEM_INVALID_ENC_IV );

            s1 += 16;
        }
#endif /* MBEDTLS_DES_C */

#if defined(MBEDTLS_AES_C)
        if( memcmp( s1, "DEK-Info: AES-", 14 ) == 0 )
        {
            if( memcmp( s1, "DEK-Info: AES-128-CBC,", 22 ) == 0 )
                enc_alg = MBEDTLS_CIPHER_AES_128_CBC;
            else if( memcmp( s1, "DEK-Info: AES-192-CBC,", 22 ) == 0 )
                enc_alg = MBEDTLS_CIPHER_AES_192_CBC;
            else if( memcmp( s1, "DEK-Info: AES-256-CBC,", 22 ) == 0 )
                enc_alg = MBEDTLS_CIPHER_AES_256_CBC;
            else
                return( MBEDTLS_ERR_PEM_UNKNOWN_ENC_ALG );

            s1 += 22;
            if( pem_get_iv( s1, pem_iv, 16 ) != 0 )
                return( MBEDTLS_ERR_PEM_INVALID_ENC_IV );

            s1 += 32;
        }
#endif /* MBEDTLS_AES_C */

        if( enc_alg == MBEDTLS_CIPHER_NONE )
            return( MBEDTLS_ERR_PEM_UNKNOWN_ENC_ALG );

        if( *s1 == '\r' ) s1++;
        if( *s1 == '\n' ) s1++;
        else return( MBEDTLS_ERR_PEM_INVALID_DATA );
#else
        return( MBEDTLS_ERR_PEM_FEATURE_UNAVAILABLE );
#endif /* MBEDTLS_MD5_C && MBEDTLS_CIPHER_MODE_CBC &&
          ( MBEDTLS_AES_C || MBEDTLS_DES_C ) */
    }

    ret = mbedtls_base64_decode( NULL, 0, &len, s1, s2 - s1 );

    if( ret == MBEDTLS_ERR_BASE64_INVALID_CHARACTER )
        return( MBEDTLS_ERR_PEM_INVALID_DATA + ret );

    if( ( buf = mbedtls_calloc( 1 * len ) ) == NULL )
        return( MBEDTLS_ERR_PEM_ALLOC_FAILED );
    
    if( ( ret = mbedtls_base64_decode( buf, len, &len, s1, s2 - s1 ) ) != 0 )
    {
        mbedtls_free( buf );
        return( MBEDTLS_ERR_PEM_INVALID_DATA + ret );
    }

    if( enc != 0 )
    {
#if defined(MBEDTLS_MD5_C) && defined(MBEDTLS_CIPHER_MODE_CBC) &&         \
    ( defined(MBEDTLS_DES_C) || defined(MBEDTLS_AES_C) )
        if( pwd == NULL )
        {
            mbedtls_free( buf );
            return( MBEDTLS_ERR_PEM_PASSWORD_REQUIRED );
        }

#if defined(MBEDTLS_DES_C)
        if( enc_alg == MBEDTLS_CIPHER_DES_EDE3_CBC )
            pem_des3_decrypt( pem_iv, buf, len, pwd, pwdlen );
        else if( enc_alg == MBEDTLS_CIPHER_DES_CBC )
            pem_des_decrypt( pem_iv, buf, len, pwd, pwdlen );
#endif /* MBEDTLS_DES_C */

#if defined(MBEDTLS_AES_C)
        if( enc_alg == MBEDTLS_CIPHER_AES_128_CBC )
            pem_aes_decrypt( pem_iv, 16, buf, len, pwd, pwdlen );
        else if( enc_alg == MBEDTLS_CIPHER_AES_192_CBC )
            pem_aes_decrypt( pem_iv, 24, buf, len, pwd, pwdlen );
        else if( enc_alg == MBEDTLS_CIPHER_AES_256_CBC )
            pem_aes_decrypt( pem_iv, 32, buf, len, pwd, pwdlen );
#endif /* MBEDTLS_AES_C */

        /*
         * The result will be ASN.1 starting with a SEQUENCE tag, with 1 to 3
         * length bytes (allow 4 to be sure) in all known use cases.
         *
         * Use that as heurisitic to try detecting password mismatchs.
         */
        if( len <= 2 || buf[0] != 0x30 || buf[1] > 0x83 )
        {
            mbedtls_free( buf );
            return( MBEDTLS_ERR_PEM_PASSWORD_MISMATCH );
        }
#else
        mbedtls_free( buf );
        return( MBEDTLS_ERR_PEM_FEATURE_UNAVAILABLE );
#endif /* MBEDTLS_MD5_C && MBEDTLS_CIPHER_MODE_CBC &&
          ( MBEDTLS_AES_C || MBEDTLS_DES_C ) */
    }

    ctx->buf = buf;
    ctx->buflen = len;

    return( 0 );
}

void mbedtls_pem_free( mbedtls_pem_context *ctx )
{
    mbedtls_free( ctx->buf );
    mbedtls_free( ctx->info );

    mbedtls_zeroize( ctx, sizeof( mbedtls_pem_context ) );
}
#endif /* MBEDTLS_PEM_PARSE_C */

#if defined(MBEDTLS_PEM_WRITE_C)
int mbedtls_pem_write_buffer( const char *header, const char *footer,
                      const unsigned char *der_data, size_t der_len,
                      unsigned char *buf, size_t buf_len, size_t *olen )
{
    int ret;
    unsigned char *encode_buf, *c, *p = buf;
    size_t len = 0, use_len, add_len = 0;

    mbedtls_base64_encode( NULL, 0, &use_len, der_data, der_len );
    add_len = strlen( header ) + strlen( footer ) + ( use_len / 64 ) + 1;

    if( use_len + add_len > buf_len )
    {
        *olen = use_len + add_len;
        return( MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL );
    }

    if( ( encode_buf = mbedtls_calloc( 1 * use_len ) ) == NULL )
        return( MBEDTLS_ERR_PEM_ALLOC_FAILED );

    if( ( ret = mbedtls_base64_encode( encode_buf, use_len, &use_len, der_data,
                               der_len ) ) != 0 )
    {
        mbedtls_free( encode_buf );
        return( ret );
    }

    memcpy( p, header, strlen( header ) );
    p += strlen( header );
    c = encode_buf;

    while( use_len )
    {
        len = ( use_len > 64 ) ? 64 : use_len;
        memcpy( p, c, len );
        use_len -= len;
        p += len;
        c += len;
        *p++ = '\n';
    }

    memcpy( p, footer, strlen( footer ) );
    p += strlen( footer );

    *p++ = '\0';
    *olen = p - buf;

    mbedtls_free( encode_buf );
    return( 0 );
}
#endif /* MBEDTLS_PEM_WRITE_C */
#endif /* MBEDTLS_PEM_PARSE_C || MBEDTLS_PEM_WRITE_C */
