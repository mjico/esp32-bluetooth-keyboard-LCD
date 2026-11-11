idf_component_register(SRCS "hello_world_main.c" "lcd_text.c" "debug_port.c" "command_history.c" "command_interpreter.cpp" "button.cpp" "bluetooth.c" "trace.c" "i2c.cpp" "sonar.cpp" "matrix.cpp" "font.c"
                    REQUIRES esp_lcd driver esp_driver_usb_serial_jtag bt nvs_flash esp_hid esp_phy)
