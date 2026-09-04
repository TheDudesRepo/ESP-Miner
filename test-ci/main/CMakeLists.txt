idf_component_register(
    SRCS "unit_test_all.c" "test_btc_price_data.c" "../../main/btc_price_data.c"
    INCLUDE_DIRS "." "../../main"
    PRIV_REQUIRES unity espressif__cjson
    WHOLE_ARCHIVE
)
