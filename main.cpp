#include <iostream>
#include <string>
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <fstream>

using namespace std;

// ================= 配置区域 =================
const string CORRECT_PASS = "admin123"; 
const string PASS_FILE = "D:\\rjkillerauthpass";
// ===========================================

// 获取Debug权限
BOOL EnableDebugPrivilege() {
    HANDLE hToken;
    LUID luid;
    TOKEN_PRIVILEGES tkp;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return FALSE;
    if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) { CloseHandle(hToken); return FALSE; }
    
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Luid = luid;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL bRet = AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, NULL);
    CloseHandle(hToken);
    return bRet;
}

// 管理员检测
BOOL IsRunAsAdmin() {
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD dwElevation = 0;
        DWORD dwSize = 0;
        if (GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS)20, &dwElevation, sizeof(dwElevation), &dwSize)) {
            CloseHandle(hToken);
            return (dwElevation != 0);
        }
        CloseHandle(hToken);
    }
    return FALSE;
}

// 提权
void ElevateNow() {
    char szPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szPath, MAX_PATH)) {
        ShellExecuteA(NULL, "runas", szPath, NULL, NULL, SW_SHOWNORMAL);
    }
}

// 【新增】解除对 I 盘的访问限制
void UnlockDriveI() {
    HKEY hKey;
    const char* subKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    
    // 1. 打开注册表项
    if (RegOpenKeyExA(HKEY_CURRENT_USER, subKey, 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        // 删除 "防止从我的电脑访问驱动器" 键值
        // 值为 0 表示不限制任何驱动器
        RegDeleteValueA(hKey, "NoViewOnDrive");
        
        // 删除 "隐藏我的电脑中的驱动器" 键值
        RegDeleteValueA(hKey, "NoDrives");
        
        RegCloseKey(hKey);
        cout << "[*] 注册表限制已清除。" << endl;
    }

    // 2. 刷新组策略 (尝试无感刷新)
    system("gpupdate /force >nul 2>&1");

    // 3. 重启资源管理器以确保更改立即生效并卸载可能存在的 DLL 劫持
    cout << "[*] 正在重启资源管理器以应用解锁..." << endl;
    
    // 先杀掉
    system("taskkill /f /im explorer.exe >nul 2>&1");
    Sleep(1000);
    
    // 再启动
    WinExec("explorer.exe", SW_SHOW);
}

// 进程列表
const char* targetProcesses[] = {
    "RJService.exe", "NetworkTool.exe", "RJAgent.exe", 
    "est-connector-helper.exe", "ESTRATool.exe", 
    "ESTRAToolUI.exe", "ESTServerGuard.exe",
    "CMApp.exe", "CMLauncher.exe", "CMService.exe", 
    "ESTRemote.exe", "LogonNotify64.exe", "RjUsbController.exe", 
    "shutdown.exe", "RJvMsgDisp.exe", "VoiAgent.exe", "ApplService.exe"
};
const int processCount = sizeof(targetProcesses) / sizeof(targetProcesses[0]);

// 强制结束进程
void KillProcessByName_API(const char* processName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe32)) {
        do {
            if (_stricmp(pe32.szExeFile, processName) == 0) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                if (hProcess != NULL) {
                    TerminateProcess(hProcess, 0);
                    CloseHandle(hProcess);
                }
            }
        } while (Process32Next(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

// 线程函数
DWORD WINAPI KillThread(LPVOID lpParam) {
    const char* processName = (const char*)lpParam;
    while (true) {
        KillProcessByName_API(processName);
        Sleep(500);
    }
    return 0;
}

int main() {
    // 1. 权限检查
    if (!IsRunAsAdmin()) {
        cout << "[提示] 正在请求管理员权限..." << endl;
        ElevateNow();
        return 0;
    }

    EnableDebugPrivilege();

    cout << "RJKiller ver1.0 Unlock Edition by zt3927" << endl;

    // 2. 密码校验
    ifstream checkFile(PASS_FILE.c_str());
    bool skipPass = checkFile.good();
    checkFile.close();

    if (!skipPass) {
        cout << "授权密码: ";
        string inputPass;
        cin >> inputPass;

        if (inputPass != CORRECT_PASS) {
            cout << "[错误] 密码错误。" << endl;
            system("pause");
            return 1;
        }
        
        ofstream createFile(PASS_FILE.c_str());
        if (createFile.is_open()) {
            createFile << "Authorized" << endl;
            createFile.close();
            string hideCmd = "attrib +s +h \"" + PASS_FILE + "\"";
            system(hideCmd.c_str());
        }
    } else {
        cout << "[信息] 已授权，跳过密码。" << endl;
    }

    // 3. 预处理服务
    cout << "[*] 正在停止底层服务..." << endl;
    system("net stop cmdriver >nul 2>&1");

    // 4. 【新增】解除驱动器限制
    UnlockDriveI();

    cout << "[*] 启动多线程API监控清理 (Ctrl+C退出)..." << endl;

    // 5. 创建监控线程
    HANDLE hThreads[20];
    
    for (int i = 0; i < processCount; i++) {
        hThreads[i] = CreateThread(NULL, 0, KillThread, (LPVOID)targetProcesses[i], 0, NULL);
    }

    WaitForMultipleObjects(processCount, hThreads, TRUE, INFINITE);

    return 0;
}
