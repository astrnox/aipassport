# Passport 随身工具箱：应用组件。
#
# 只编译本产品实际使用的源码。模板自带的 demo_*.c 仍保留在仓库中供上游示例与
# 主机测试使用，但本产品已用 ui/ui_app.c 的全新界面替换了 demo 测试菜单，因此不
# 再把 demo 源码链入固件（见 AGENTS.md 的强制 UI 重新设计规则）。
idf_component_register(
    SRCS "main.c"
         "app_state.c"
         "logic/app_badge.c"
         "logic/app_esports.c"
         "logic/app_pomodoro.c"
         "logic/app_qr.c"
         "logic/app_reminder.c"
         "logic/app_routine.c"
         "logic/app_text.c"
         "logic/app_time.c"
         "logic/app_totp.c"
         "net/app_blufi.c"
         "net/app_net.c"
         "ui/ui_app.c"
         "ui/ui_esports.c"
         "ui/ui_focus.c"
         "ui/ui_home.c"
         "ui/ui_identity.c"
         "ui/ui_routine.c"
         "ui/ui_settings.c"
         "ui/ui_sound.c"
         "ui/ui_theme.c"
         "ui/ui_time.c"
         "ui/ui_timeedit.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             lvgl esp_http_client esp_http_server json mbedtls
)

# 中文字形：由 tools/gen_fonts.py 生成，覆盖常用汉字集。生成的是普通 C 源文件，
# 这里直接把它们登记为组件源码，与上面的 SRCS 一起编译（不要重复 #include 这些 .c）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_12.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_20.c"
)
