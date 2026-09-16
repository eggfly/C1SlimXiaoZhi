# Host-side tests. They cover the parts that are pure logic and therefore
# testable without the device: protocol framing, the state machine, text
# layout, settings persistence and the Ogg reader.
#
# Anything that needs the ES8326, the e-paper panel or the keyboard is not
# covered here and is listed as untested in docs/STATUS.md.

add_executable(c1xz_tests
    test_main.cc
    test_url.cc
    test_settings.cc
    test_state_machine.cc
    test_gfx.cc
    test_opus_roundtrip.cc
    test_ogg.cc
    test_mcp.cc)
target_link_libraries(c1xz_tests PRIVATE c1xz_core)

add_test(NAME unit COMMAND c1xz_tests)
