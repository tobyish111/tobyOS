/* dll28m -- Track C, W3: built only so win-dllm28 can link against it; it is
 * deliberately NOT in the initrd. */
__declspec(dllexport) int m_value(void) { return 9; }
