string TestPrefix() { return "wc_"; }
string NormalizeMode(const string &in mode) { return GPT_WC_NormalizeThinkingMode(mode); }
string ChatPayload(const string &in model, const string &in system, const string &in user, const string &in mode) {
    return GPT_WC_BuildChatPayload(model, system, user, mode);
}
void ResetVariant() {}
void TestVariant() {
    Fresh("auto");
    HostSaveString("gpt_thinking_mode", "disabled");
    RefreshConfiguration();
    AssertMode("auto");
    Check(HostLoadString("gpt_thinking_mode", "") == "disabled", "variants keep independent settings");
}
