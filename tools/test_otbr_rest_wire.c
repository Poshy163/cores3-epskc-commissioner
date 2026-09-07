#include <assert.h>
#include <string.h>

#include "../main/otbr_rest_wire.h"

int main(void)
{
    char body[8] = { 'a', 'b', 'c' };
    assert(otbr_rest_terminate_body(body, 3, sizeof(body)));
    assert(strcmp(body, "abc") == 0);
    char embedded_nul[5] = { 'a', '\0', 'b', 'c' };
    assert(!otbr_rest_terminate_body(embedded_nul, 4, sizeof(embedded_nul)));
    assert(!otbr_rest_terminate_body(body, sizeof(body), sizeof(body)));

    char json[64];
    assert(otbr_rest_quote_json_string("plain", json, sizeof(json)));
    assert(strcmp(json, "\"plain\"") == 0);
    assert(otbr_rest_quote_json_string("a\"b\\c\n", json, sizeof(json)));
    assert(strcmp(json, "\"a\\\"b\\\\c\\u000a\"") == 0);
    assert(!otbr_rest_quote_json_string("abc", json, 5));

    char bare[] = "  0a10";
    assert(strcmp(otbr_rest_parse_dataset_hex_body(bare), "0a10") == 0);
    char quoted[] = " \"0a10\" \t";
    assert(strcmp(otbr_rest_parse_dataset_hex_body(quoted), "0a10") == 0);
    char missing_quote[] = "\"0a10";
    assert(otbr_rest_parse_dataset_hex_body(missing_quote) == NULL);
    char trailing[] = "\"0a10\" garbage";
    assert(otbr_rest_parse_dataset_hex_body(trailing) == NULL);
    char escaped[] = "\"0a\\10\"";
    assert(otbr_rest_parse_dataset_hex_body(escaped) == NULL);

    bool on = false;
    assert(otbr_rest_parse_toggle("enable", &on) && on);
    assert(otbr_rest_parse_toggle(" \"disable\" ", &on) && !on);
    assert(!otbr_rest_parse_toggle("\"enable", &on));
    assert(!otbr_rest_parse_toggle("\"\"enable\"", &on));

    long lifetime = 1;
    long port = 2;
    assert(otbr_rest_parse_epskc_options("", &lifetime, &port));
    assert(otbr_rest_parse_epskc_options("{\"lifetime\":600000,\"port\":65535}",
                                         &lifetime, &port));
    assert(lifetime == 600000 && port == 65535);
    assert(!otbr_rest_parse_epskc_options("{\"port\":+1}", &lifetime, &port));
    assert(!otbr_rest_parse_epskc_options("{\"port\":01}", &lifetime, &port));
    assert(!otbr_rest_parse_epskc_options("{\"port\":1,}", &lifetime, &port));
    assert(!otbr_rest_parse_epskc_options("{\"port\":1,\"port\":2}", &lifetime, &port));
    assert(!otbr_rest_parse_epskc_options("{\"port\":999999999999999999999999}",
                                          &lifetime, &port));
    /* Syntax parsing intentionally leaves endpoint-specific bounds checks to
     * the handler, but preserves out-of-range values for those checks. */
    assert(otbr_rest_parse_epskc_options("{\"port\":65536,\"lifetime\":600001}",
                                         &lifetime, &port));
    assert(port == 65536 && lifetime == 600001);
    return 0;
}
