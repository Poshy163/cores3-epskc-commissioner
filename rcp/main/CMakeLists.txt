idf_component_register(SRCS "esp_ot_rcp.c" "ack_length_fix.c"
                       INCLUDE_DIRS ".")

# Route the 802.15.4 driver's ACK hand-off through ack_length_fix.c. The
# driver's archive is linked after main's, so -u pulls the wrapper in.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=ieee802154_inner_transmit_done"
    "-u __wrap_ieee802154_inner_transmit_done")
