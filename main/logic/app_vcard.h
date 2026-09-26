// main/logic/app_vcard.h —— 联系人名片（vCard 3.0）文本生成（与硬件无关）。
//
// 用途：把姓名、单位、电话、邮箱这些"见面要交换的信息"编成一段 vCard 文本，再交给
// app_qr 画成二维码——对方用手机相机一扫就能存进通讯录，不必手输十一位号码。
//
// 为什么是文本生成而不是解码：设备只有三个按键，输不了联系方式；输入放在手机上的
// 配置页，这里负责把字段拼成标准文本，并在二维码容量放不下时按重要性取舍。
//
// 容量取舍：二维码内容上限为 APP_QR_MAX_BYTES（120 字节），一张带单位、职务、电话、
// 邮箱的名片很容易超出。放不下时按"网址 → 职务 → 单位"的顺序丢弃可选字段，姓名、
// 电话与邮箱留到最后——它们才是扫名片真正要拿到的东西。丢弃的个数由 *dropped_fields
// 如实回传，界面据此告知用户，而不是悄悄少存几项。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define APP_VCARD_NAME_LEN  30
#define APP_VCARD_ORG_LEN   36
#define APP_VCARD_TITLE_LEN 24
#define APP_VCARD_TEL_LEN   24
#define APP_VCARD_EMAIL_LEN 40
#define APP_VCARD_URL_LEN   40

typedef struct {
    char name[APP_VCARD_NAME_LEN];    // 姓名，必填
    char org[APP_VCARD_ORG_LEN];      // 单位/学校
    char title[APP_VCARD_TITLE_LEN];  // 职务/专业
    char tel[APP_VCARD_TEL_LEN];      // 电话
    char email[APP_VCARD_EMAIL_LEN];  // 邮箱
    char url[APP_VCARD_URL_LEN];      // 网址
} app_vcard_t;

// 生成 vCard 文本，写入 out（始终以 NUL 结尾），返回写入的字节数（不含终止符）。
// 姓名必填，任一字段不是合法 UTF-8、或连姓名+电话+邮箱都放不进 cap 时返回 -1。
// dropped_fields 非空时写入因容量被丢弃的字段个数（0..3）。
int app_vcard_build(const app_vcard_t *card, char *out, size_t cap, int *dropped_fields);
