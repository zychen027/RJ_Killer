#include <iostream>
#include <string>
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <fstream>
#include <winsvc.h>
#include <wincrypt.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "Advapi32.lib")

using namespace std;

// ================= 配置区域 =================
const string CORRECT_PASS = "admin123";          // 授权密码
const string PASS_FILE = "D:\\rjkillerauthpass"; // 授权文件路径

const bool CREATE_AUTH_FILE = true;  // 【开关】是否创建授权文件 (false则每次运行都需输密码)
const bool USE_HWID_BIND = true;     // 【开关】是否启用HWID硬件绑定 (false则只验证密码，不验证机器)

// ===========================================

// --- 辅助函数 ---
string BytesToHex(BYTE* data, size_t len) {
    string hex; char buf[3];
    for (size_t i = 0; i < len; i++) { wsprintf(buf, "%02x", data[i]); hex += buf; }
    return hex;
}

string GetSHA256(const string& input) {
    HCRYPTPROV hProv = 0; HCRYPTHASH hHash = 0;
    BYTE rgbHash[32]; DWORD cbHash = 32; string result = "";
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) { CryptReleaseContext(hProv, 0); return ""; }
    if (CryptHashData(hHash, (const BYTE*)input.c_str(), input.size(), 0)) {
        if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) result = BytesToHex(rgbHash, cbHash);
    }
    CryptDestroyHash(hHash); CryptReleaseContext(hProv, 0);
    return result;
}

// --- HWID 生成 ---
string GenerateHWID() {
    string rawFeatures = "";
    char buffer[512];

    // 1. D盘序列号
    DWORD volSerial = 0;
    if (GetVolumeInformationA("D:\\", NULL, 0, &volSerial, NULL, NULL, NULL, 0)) {
        wsprintf(buffer, "[DISK:%08X]", volSerial);
        rawFeatures += buffer;
    }

    // 2. D盘文件特征 (排序)
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA("D:\\*", &findData);
    vector<string> fileList;
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (findData.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY)) continue;
            fileList.push_back(string(findData.cFileName) + "_" + to_string(findData.nFileSizeLow));
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }

    sort(fileList.begin(), fileList.end()); 
    for (size_t i = 0; i < fileList.size() && i < 3; i++) {
        rawFeatures += "[FILE:" + fileList[i] + "]";
    }

    // 3. 计算机名
    char compName[256]; DWORD size = 256;
    if (GetComputerNameA(compName, &size)) {
        rawFeatures += "[PC:" + string(compName) + "]";
    }

    return GetSHA256(rawFeatures);
}

// --- 权限与服务管理 ---
BOOL EnableDebugPrivilege() {
    HANDLE hToken; LUID luid; TOKEN_PRIVILEGES tkp;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return FALSE;
    if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) { CloseHandle(hToken); return FALSE; }
    tkp.PrivilegeCount = 1; tkp.Privileges[0].Luid = luid; tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL bRet = AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, NULL);
    CloseHandle(hToken); return bRet;
}

BOOL IsRunAsAdmin() {
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD dwElevation = 0; DWORD dwSize = 0;
        if (GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS)20, &dwElevation, sizeof(dwElevation), &dwSize)) {
            CloseHandle(hToken); return (dwElevation != 0);
        }
        CloseHandle(hToken);
    }
    return FALSE;
}

void ElevateNow() {
    char szPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szPath, MAX_PATH)) ShellExecuteA(NULL, "runas", szPath, NULL, NULL, SW_SHOWNORMAL);
}

void ForceStopServices() {
    cout << "[*] 正在扫描并停止系统服务..." << endl;
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return;
    DWORD dwBytesNeeded, dwServicesReturned, dwResumeHandle = 0;
    EnumServicesStatusA(hSCM, SERVICE_DRIVER | SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &dwBytesNeeded, &dwServicesReturned, &dwResumeHandle);
    BYTE* pBuffer = new BYTE[dwBytesNeeded];
    LPENUM_SERVICE_STATUSA pServices = (LPENUM_SERVICE_STATUSA)pBuffer;
    if (EnumServicesStatusA(hSCM, SERVICE_DRIVER | SERVICE_WIN32, SERVICE_STATE_ALL, pServices, dwBytesNeeded, &dwBytesNeeded, &dwServicesReturned, &dwResumeHandle)) {
        for (DWORD i = 0; i < dwServicesReturned; i++) {
            string svcName = pServices[i].lpServiceName;
            if (svcName.find("RJ") != string::npos || svcName.find("CM") != string::npos || svcName.find("EST") != string::npos) {
                cout << "    [>] 停止服务: " << svcName << endl;
                SC_HANDLE hService = OpenServiceA(hSCM, pServices[i].lpServiceName, SERVICE_STOP);
                if (hService) { SERVICE_STATUS status; ControlService(hService, SERVICE_CONTROL_STOP, &status); CloseServiceHandle(hService); }
            }
        }
    }
    delete[] pBuffer; CloseServiceHandle(hSCM);
}

void UnlockDriveI() {
    cout << "[*] 执行解锁操作..." << endl;
    system("reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\" /v NoViewOnDrive /f");
    system("reg delete \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\" /v NoViewOnDrive /f");
    system("taskkill /f /im explorer.exe"); Sleep(2000);
    WinExec("explorer.exe", SW_SHOW);
}

// --- 进程清理 ---
const char* targetProcesses[] = {
    "RJService.exe", "NetworkTool.exe", "RJAgent.exe", "est-connector-helper.exe", 
    "ESTRATool.exe", "ESTRAToolUI.exe", "ESTServerGuard.exe", "CMApp.exe", 
    "CMLauncher.exe", "CMService.exe", "ESTRemote.exe", "LogonNotify64.exe", 
    "RjUsbController.exe", "shutdown.exe", "RJvMsgDisp.exe", "VoiAgent.exe"
};
const int processCount = sizeof(targetProcesses) / sizeof(targetProcesses[0]);

void KillProcessByName_API(const char* processName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 pe32; pe32.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(hSnapshot, &pe32)) {
        do {
            if (_stricmp(pe32.szExeFile, processName) == 0) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                if (hProcess) { TerminateProcess(hProcess, 0); CloseHandle(hProcess); }
            }
        } while (Process32Next(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

DWORD WINAPI KillThread(LPVOID lpParam) {
    const char* processName = (const char*)lpParam;
    while (true) { KillProcessByName_API(processName); Sleep(1000); }
    return 0;
}

int main() {
    if (!IsRunAsAdmin()) { cout << "[提示] 正在请求管理员权限..." << endl; ElevateNow(); return 0; }
    EnableDebugPrivilege();
    cout << "RJKiller ver1.5 Config Edition by zt3927" << endl;

    // 1. HWID 处理
    string currentHWID = "";
    if (USE_HWID_BIND) {
        currentHWID = GenerateHWID();
        cout << "[*] 模式: HWID绑定 | 当前ID: " << currentHWID << endl;
    } else {
        cout << "[*] 模式: 密码验证 (无硬件绑定)" << endl;
    }

    // 2. 验证逻辑
    bool isAuthorized = false;
    
    // 只有当允许创建文件时，才尝试读取文件验证
    if (CREATE_AUTH_FILE) {
        ifstream checkFile(PASS_FILE.c_str());
        if (checkFile.is_open()) {
            string line1, line2;
            getline(checkFile, line1);
            
            if (USE_HWID_BIND) {
                getline(checkFile, line2); // 读取第二行 HWID
                if (line1 == "Authorized" && line2 == currentHWID) {
                    isAuthorized = true;
                    cout << "[信息] 授权文件验证通过。" << endl;
                } else {
                    cout << "[警告] 授权文件无效或HWID不匹配。" << endl;
                }
            } else {
                // 不绑定HWID时，只检查第一行
                if (line1 == "Authorized") {
                    isAuthorized = true;
                    cout << "[信息] 授权文件验证通过。" << endl;
                }
            }
            checkFile.close();
        }
    }

    // 3. 密码输入与写入
    if (!isAuthorized) {
        cout << "授权密码: ";
        string inputPass;
        cin >> inputPass;

        if (inputPass != CORRECT_PASS) {
            cout << "[错误] 密码错误。" << endl;
            system("pause");
            return 1;
        }

        // 密码正确，判断是否需要保存文件
        if (CREATE_AUTH_FILE) {
            // 去除属性以确保能写入
            SetFileAttributesA(PASS_FILE.c_str(), FILE_ATTRIBUTE_NORMAL);
            
            ofstream createFile(PASS_FILE.c_str());
            if (createFile.is_open()) {
                createFile << "Authorized" << endl;
                if (USE_HWID_BIND) {
                    createFile << currentHWID << endl; // 写入 HWID
                }
                createFile.close();
                
                // 重新隐藏
                SetFileAttributesA(PASS_FILE.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
                cout << "[成功] 授权已保存。" << endl;
            }
        } else {
            cout << "[信息] 密码正确 (本次运行有效)。" << endl;
        }
    }

    // 4. 核心清理
    ForceStopServices();
    UnlockDriveI();
    
    cout << "================================================" << endl;
    cout << "[*] 启动多线程API监控清理..." << endl;

    HANDLE hThreads[20];
    for (int i = 0; i < processCount; i++) hThreads[i] = CreateThread(NULL, 0, KillThread, (LPVOID)targetProcesses[i], 0, NULL);
    WaitForMultipleObjects(processCount, hThreads, TRUE, INFINITE);

    return 0;
}
