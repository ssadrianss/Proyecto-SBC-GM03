idf_component_register(SRCS "app_main.c" "ds18b20.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES telegram_certificate.pem)
