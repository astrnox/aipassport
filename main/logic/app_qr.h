// main/logic/app_qr.h —— 字节模式 QR 码生成（纠错等级 M，版本 1..10），与硬件无关。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_QR_MIN_SIZE 21   // 版本 1
#define APP_QR_MAX_SIZE 57   // 版本 10
#define APP_QR_MAX_BYTES 120 // 本产品允许的二维码内容上限

// 生成二维码。modules 为 size*size 的模块矩阵，行优先，1=黑 0=白。
// modules_cap 必须 >= APP_QR_MAX_SIZE*APP_QR_MAX_SIZE (3249)。
// 成功返回 true 并写 *size_out；内容过长或参数非法返回 false。
bool app_qr_encode(const char *text, uint8_t *modules, size_t modules_cap, int *size_out);

// 该字节长度需要的最小二维码边长（模块数）；超出能力返回 -1。
int app_qr_size_for(size_t text_len);
