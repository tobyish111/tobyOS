/* win-dllm28 -- Track C, W3: imports from dll28m.dll, which is not on disk.
 * Windows refuses to start the process: STATUS_DLL_NOT_FOUND (0xC0000135),
 * saying which DLL. Reaching main is a FAIL (exit 1). */
__declspec(dllimport) int m_value(void);

int main(void) { return m_value() == 9 ? 1 : 2; }
