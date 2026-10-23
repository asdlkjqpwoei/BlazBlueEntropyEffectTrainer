#include <cstring>
#include <exception>
#include <iostream>

int RunCoreTests();
int RunPuertsRuntimeTests();
int RunHotkeyTests();
int RunWindowTests();
int RunWindowIntegration();
int RunLocalizationTests();
int RunRuntimeProbe(int ArgumentCount, char** Arguments);
int RunEngineProbe(int ArgumentCount, char** Arguments);
int RunScanProbe(int ArgumentCount, char** Arguments);
int RunExchangeProbe(int ArgumentCount, char** Arguments);
int RunExchangeWatch(int ArgumentCount, char** Arguments);

int main(int ArgumentCount, char** Arguments)
{
    try
    {
        if (ArgumentCount > 1)
        {
            if (std::strcmp(Arguments[1], "--window-integration") == 0)
                return RunWindowIntegration();
            if (std::strcmp(Arguments[1], "--runtime-probe") == 0)
                return RunRuntimeProbe(ArgumentCount - 1, Arguments + 1);
            if (std::strcmp(Arguments[1], "--engine-probe") == 0)
                return RunEngineProbe(ArgumentCount - 1, Arguments + 1);
            if (std::strcmp(Arguments[1], "--scan-probe") == 0)
                return RunScanProbe(ArgumentCount - 1, Arguments + 1);
            if (std::strcmp(Arguments[1], "--exchange-probe") == 0)
                return RunExchangeProbe(ArgumentCount - 1, Arguments + 1);
            if (std::strcmp(Arguments[1], "--exchange-watch") == 0)
                return RunExchangeWatch(ArgumentCount - 1, Arguments + 1);
            std::cerr << "Unknown test/diagnostic mode.\n";
            return 1;
        }
        const int CoreResult = RunCoreTests();
        const int RuntimeResult = RunPuertsRuntimeTests();
        const int HotkeyResult = RunHotkeyTests();
        const int WindowResult = RunWindowTests();
        const int LanguageResult = RunLocalizationTests();
        if (CoreResult || RuntimeResult || HotkeyResult || WindowResult || LanguageResult)
            return 1;
        std::cout << "Core, runtime ownership, localization, hotkeys, and DPI/window tests passed.\n";
        return 0;
    }
    catch (const std::exception& Error)
    {
        std::cerr << "Test execution failed: " << Error.what() << '\n';
        return 1;
    }
}
