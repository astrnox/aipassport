// tests/test_app_totp.c —— app_totp 的主机侧单元测试（RFC 6238 附录 B 向量）。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_totp.h"

// 构造 RFC 6238 附录 B 的 SHA-1 测试账户：密钥 ASCII "12345678901234567890"，
// 8 位口令，周期 30 秒。
static app_totp_account_t make_sha1_account(void)
{
    app_totp_account_t acct;
    memset(&acct, 0, sizeof(acct));
    memcpy(acct.label, "RFC", 4);

    size_t len = 0;
    bool ok = app_totp_base32_decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",
                                     acct.secret, sizeof(acct.secret), &len);
    assert(ok);
    acct.secret_len = len;
    acct.digits = 8;
    acct.period = 30;
    acct.algo = APP_TOTP_ALGO_SHA1;
    return acct;
}

int main(void)
{
    // ---- Base32 解码 ----
    uint8_t raw[APP_TOTP_MAX_SECRET_BYTES];
    size_t raw_len = 0;
    assert(app_totp_base32_decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",
                                  raw, sizeof(raw), &raw_len));
    assert(raw_len == 20);
    assert(memcmp(raw, "12345678901234567890", 20) == 0);

    // 大小写、空格、'-' 与 '=' 填充都应被容忍。
    uint8_t raw2[APP_TOTP_MAX_SECRET_BYTES];
    size_t raw2_len = 0;
    assert(app_totp_base32_decode("gezd gnbv-gy3tqojqgezdgnbvgy3tqojq==",
                                  raw2, sizeof(raw2), &raw2_len));
    assert(raw2_len == 20);
    assert(memcmp(raw2, raw, 20) == 0);

    // 非法字符与空输入必须失败。
    size_t bad_len = 0;
    assert(!app_totp_base32_decode("0189", raw, sizeof(raw), &bad_len));
    assert(!app_totp_base32_decode("", raw, sizeof(raw), &bad_len));
    assert(!app_totp_base32_decode("   --  ", raw, sizeof(raw), &bad_len));

    // ---- RFC 6238 附录 B：SHA-1 测试向量 ----
    app_totp_account_t acct = make_sha1_account();
    char code[16];

    assert(app_totp_code(&acct, 59ULL, code, sizeof(code)));
    assert(strcmp(code, "94287082") == 0);
    assert(app_totp_code(&acct, 1111111109ULL, code, sizeof(code)));
    assert(strcmp(code, "07081804") == 0);
    assert(app_totp_code(&acct, 1111111111ULL, code, sizeof(code)));
    assert(strcmp(code, "14050471") == 0);
    assert(app_totp_code(&acct, 1234567890ULL, code, sizeof(code)));
    assert(strcmp(code, "89005924") == 0);
    assert(app_totp_code(&acct, 2000000000ULL, code, sizeof(code)));
    assert(strcmp(code, "69279037") == 0);
    assert(app_totp_code(&acct, 20000000000ULL, code, sizeof(code)));
    assert(strcmp(code, "65353130") == 0);

    // ---- 剩余秒数 ----
    assert(app_totp_remaining(&acct, 59ULL) == 1);
    assert(app_totp_remaining(&acct, 0ULL) == 30);

    // ---- 分组显示 ----
    char formatted[16];
    assert(app_totp_format("482915", formatted, sizeof(formatted)) == 7);
    assert(strcmp(formatted, "482 915") == 0);
    assert(app_totp_format("48291537", formatted, sizeof(formatted)) == 9);
    assert(strcmp(formatted, "4829 1537") == 0);
    assert(app_totp_format("12345", formatted, sizeof(formatted)) == -1);

    // ---- otpauth URI 解析 ----
    app_totp_account_t parsed;
    const char *uri =
        "otpauth://totp/ACME%20Co:alice%40example.com"
        "?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=ACME"
        "&digits=8&period=60&algorithm=SHA256";
    assert(app_totp_parse_uri(uri, &parsed));
    assert(strcmp(parsed.label, "alice@example.com") == 0);
    assert(parsed.digits == 8);
    assert(parsed.period == 60);
    assert(parsed.algo == APP_TOTP_ALGO_SHA256);
    assert(parsed.secret_len == 20);
    assert(memcmp(parsed.secret, "12345678901234567890", 20) == 0);

    // 省略可选参数时使用默认值。
    app_totp_account_t defaulted;
    assert(app_totp_parse_uri("otpauth://totp/alice?secret=GEZDGNBVGY3TQOJQ", &defaulted));
    assert(strcmp(defaulted.label, "alice") == 0);
    assert(defaulted.digits == 6);
    assert(defaulted.period == 30);
    assert(defaulted.algo == APP_TOTP_ALGO_SHA1);

    // 缺少 secret 必须失败。
    assert(!app_totp_parse_uri("otpauth://totp/ACME:alice?digits=6", &parsed));

    puts("test_app_totp: PASS");
    return 0;
}
