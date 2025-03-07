/*
 * Copyright (c) 2018-2019 Confetti Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
*/

#ifdef _WIN32

#if !defined(_DURANGO)
#include "shlobj.h"
#include "commdlg.h"
#endif

#include "../Interfaces/IFileSystem.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/IOperatingSystem.h"
#include "../Interfaces/IThread.h"
#include "../Interfaces/IMemory.h"

#if defined(DIRECT3D12)
#define RESOURCE_DIR "Shaders/D3D12"
#elif defined(DIRECT3D11)
#define RESOURCE_DIR "Shaders/D3D11"
#elif defined(VULKAN)
#define RESOURCE_DIR "Shaders/Vulkan"
#else
#define RESOURCE_DIR "Shaders"
#endif

const char* pszRoots[FSR_Count] = {
	RESOURCE_DIR "/Binary/",    // FSR_BinShaders
	RESOURCE_DIR "/",           // FSR_SrcShaders
	"Textures/",                // FSR_Textures
	"Meshes/",                  // FSR_Meshes
	"Fonts/",                   // FSR_Builtin_Fonts
	"GPUCfg/",                  // FSR_GpuConfig
	"Animation/",               // FSR_Animation
	"Audio/",                   // FSR_Audio
	"",                         // FSR_OtherFiles
};

FileHandle open_file(const char* filename, const char* flags)
{
	FILE* fp;
	fopen_s(&fp, filename, flags);
	return fp;
}

bool close_file(FileHandle handle) { return (fclose((::FILE*)handle) == 0); }

void flush_file(FileHandle handle) { fflush((::FILE*)handle); }

size_t read_file(void* buffer, size_t byteCount, FileHandle handle) { return fread(buffer, 1, byteCount, (::FILE*)handle); }

bool seek_file(FileHandle handle, long offset, int origin) { return fseek((::FILE*)handle, offset, origin) == 0; }

long tell_file(FileHandle handle) { return ftell((::FILE*)handle); }

size_t write_file(const void* buffer, size_t byteCount, FileHandle handle) { return fwrite(buffer, 1, byteCount, (::FILE*)handle); }

time_t get_file_last_modified_time(const char* _fileName)
{
	struct stat fileInfo = {0};

	stat(_fileName, &fileInfo);
	return fileInfo.st_mtime;
}

time_t get_file_last_accessed_time(const char* _fileName)
{
	struct stat fileInfo = {0};

	stat(_fileName, &fileInfo);
	return fileInfo.st_atime;
}

time_t get_file_creation_time(const char* _fileName)
{
	struct stat fileInfo = {0};

	stat(_fileName, &fileInfo);
	return fileInfo.st_ctime;
}

eastl::string get_current_dir()
{
	char curDir[MAX_PATH];
	GetCurrentDirectoryA(MAX_PATH, curDir);
	return eastl::string(curDir);
}

eastl::string get_exe_path()
{
	char exeName[MAX_PATH];
	exeName[0] = 0;
	GetModuleFileNameA(0, exeName, MAX_PATH);
	return eastl::string(exeName);
}

eastl::string get_app_prefs_dir(const char* org, const char* app)
{
	/*
	* Vista and later has a new API for this, but SHGetFolderPath works there,
	*  and apparently just wraps the new API. This is the new way to do it:
	*
	*	SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE,
	*						 NULL, &wszPath);
	*/

	char   path[MAX_PATH];
	size_t new_wpath_len = 0;
	BOOL   api_result = FALSE;

	if (!SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, path)))
	{
		return NULL;
	}

	new_wpath_len = strlen(org) + strlen(app) + strlen(path) + 3;

	if ((new_wpath_len + 1) > MAX_PATH)
	{
		return NULL;
	}

	strcat(path, "\\");
	strcat(path, org);

	api_result = CreateDirectoryA(path, NULL);
	if (api_result == FALSE)
	{
		if (GetLastError() != ERROR_ALREADY_EXISTS)
		{
			return NULL;
		}
	}

	strcat(path, "\\");
	strcat(path, app);

	api_result = CreateDirectoryA(path, NULL);
	if (api_result == FALSE)
	{
		if (GetLastError() != ERROR_ALREADY_EXISTS)
		{
			return NULL;
		}
	}

	strcat(path, "\\");
	return eastl::string(path);
}

eastl::string get_user_documents_dir()
{
	char pathName[MAX_PATH];
	pathName[0] = 0;
	SHGetSpecialFolderPathA(0, pathName, CSIDL_PERSONAL, 0);
	return eastl::string(pathName);
}

void set_current_dir(const char* path) { SetCurrentDirectoryA(path); }

void get_files_with_extension(const char* dir, const char* ext, eastl::vector<eastl::string>& filesOut)
{
	eastl::string  path = FileSystem::GetNativePath(FileSystem::AddTrailingSlash(dir));
	WIN32_FIND_DATAA fd;
	HANDLE           hFind = ::FindFirstFileA((path + "*" + ext).c_str(), &fd);
	size_t           fileIndex = filesOut.size();
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			filesOut.resize(fileIndex + 1);
			//copy the strings to avoid the memory being cleaned up by windows.
			filesOut[fileIndex] = "";
			filesOut[fileIndex++] = path + fd.cFileName;
		} while (::FindNextFileA(hFind, &fd));
		::FindClose(hFind);
	}
}

void get_sub_directories(const char* dir, eastl::vector<eastl::string>& subDirectoriesOut)
{
	eastl::string  path = FileSystem::GetNativePath(FileSystem::AddTrailingSlash(dir));
	WIN32_FIND_DATAA fd;
	HANDLE           hFind = ::FindFirstFileA((path + "*").c_str(), &fd);
	size_t           fileIndex = subDirectoriesOut.size();
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			// skip files, ./ and ../
			if (!strchr(fd.cFileName, '.'))
			{
				subDirectoriesOut.resize(fileIndex + 1);
				//copy the strings to avoid the memory being cleaned up by windows.
				subDirectoriesOut[fileIndex] = "";
				subDirectoriesOut[fileIndex++] = path + fd.cFileName;
			}
		} while (::FindNextFileA(hFind, &fd));
		::FindClose(hFind);
	}
}

bool copy_file(const char* src, const char* dst) { return CopyFileA(src, dst, FALSE) ? true : false; }

static void
	FormatFileExtensionsFilter(const char* fileDesc, const eastl::vector<eastl::string>& extFiltersIn, eastl::string& extFiltersOut)
{
	extFiltersOut = fileDesc;
	extFiltersOut.push_back('\0');
	for (size_t i = 0; i < extFiltersIn.size(); ++i)
	{
		eastl::string ext = extFiltersIn[i];
		if (ext.size() && ext[0] == '.')
			ext = (ext.begin() + 1);
		extFiltersOut += "*.";
		extFiltersOut += ext;
		if (i != extFiltersIn.size() - 1)
			extFiltersOut += ";";
		else
			extFiltersOut.push_back('\0');
	}
}

void open_file_dialog(
	const char* title, const char* dir, FileDialogCallbackFn callback, void* userData, const char* fileDesc,
	const eastl::vector<eastl::string>& fileExtensions)
{
	eastl::string extFilter;
	FormatFileExtensionsFilter(fileDesc, fileExtensions, extFilter);
	OPENFILENAMEA ofn;                 // common dialog box structure
	char          szFile[MAX_PATH];    // buffer for file name
									   // Initialize OPENFILENAME
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lpstrTitle = title;
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	ofn.lpstrFile = szFile;
	// Set lpstrFile[0] to '\0' so that GetOpenFileName does not
	// use the contents of szFile to initialize itself.
	ofn.lpstrFile[0] = '\0';
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = extFilter.c_str();
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = dir;
	ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

	if (::GetOpenFileNameA(&ofn) == TRUE)
	{
		callback(szFile, userData);
	}
}

void save_file_dialog(
	const char* title, const char* dir, FileDialogCallbackFn callback, void* userData, const char* fileDesc,
	const eastl::vector<eastl::string>& fileExtensions)
{
	eastl::string extFilter;
	FormatFileExtensionsFilter(fileDesc, fileExtensions, extFilter);
	OPENFILENAMEA ofn;
	// buffer for file name
	char szFile[MAX_PATH];
	// Initialize OPENFILENAME
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lpstrTitle = title;
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	ofn.lpstrFile = szFile;
	// Set lpstrFile[0] to '\0' so that GetOpenFileName does not
	// use the contents of szFile to initialize itself.
	ofn.lpstrFile[0] = '\0';
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = extFilter.c_str();
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = dir;
	ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR;

	if (::GetSaveFileNameA(&ofn) == TRUE)
	{
		callback(szFile, userData);
	}
}

struct FileSystem::Watcher::Data
{
	eastl::string  mWatchDir;
	DWORD            mNotifyFilter;
	FileSystem::Watcher::Callback    mCallback;
	HANDLE           hExitEvt;
	ThreadDesc       mThreadDesc;
	ThreadHandle     mThread;
	volatile int     mRun;
};

static void fswThreadFunc(void* data)
{
	FileSystem::Watcher::Data* fs = (FileSystem::Watcher::Data*)data;

	HANDLE hDir = CreateFileA(
		fs->mWatchDir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	HANDLE hEvt = ::CreateEvent(NULL, TRUE, FALSE, NULL);

	BYTE       notifyBuffer[1024];
	OVERLAPPED ovl = { 0 };
	ovl.hEvent = hEvt;

	while (fs->mRun)
	{
		DWORD dwBytesReturned = 0;
		ResetEvent(hEvt);
		if (ReadDirectoryChangesW(hDir, &notifyBuffer, sizeof(notifyBuffer), TRUE, fs->mNotifyFilter, NULL, &ovl, NULL) == 0)
		{
			break;
		}

		HANDLE pHandles[2] = { hEvt, fs->hExitEvt };
		WaitForMultipleObjects(2, pHandles, FALSE, INFINITE);

		if (!fs->mRun)
		{
			break;
		}

		GetOverlappedResult(hDir, &ovl, &dwBytesReturned, FALSE);

		char  fileName[MAX_PATH * 4];
		DWORD offset = 0;
		BYTE* p = notifyBuffer;
		for (;;)
		{
			FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(p);
			memset(fileName, '\0', sizeof(fileName));
			WideCharToMultiByte(CP_UTF8, 0, fni->FileName, fni->FileNameLength / sizeof(WCHAR), fileName, sizeof(fileName), NULL, NULL);
			eastl::string path = fs->mWatchDir + fileName;
			uint32_t        action = 0;
			switch (fni->Action)
			{
				case FILE_ACTION_ADDED: action = FileSystem::Watcher::EVENT_CREATED; break;
				case FILE_ACTION_MODIFIED:
					if (fs->mNotifyFilter & FILE_NOTIFY_CHANGE_LAST_WRITE)
						action = FileSystem::Watcher::EVENT_MODIFIED;
					if (fs->mNotifyFilter & FILE_NOTIFY_CHANGE_LAST_ACCESS)
						action = FileSystem::Watcher::EVENT_ACCESSED;
					break;
				case FILE_ACTION_REMOVED: action = FileSystem::Watcher::EVENT_DELETED; break;
				case FILE_ACTION_RENAMED_NEW_NAME: action = FileSystem::Watcher::EVENT_CREATED; break;
				case FILE_ACTION_RENAMED_OLD_NAME: action = FileSystem::Watcher::EVENT_DELETED; break;
				default: break;
			}
			fs->mCallback(path.c_str(), action);
			if (!fni->NextEntryOffset)
				break;
			p += fni->NextEntryOffset;
		}
	}

	CloseHandle(hDir);
	CloseHandle(hEvt);
};


FileSystem::Watcher::Watcher(const char* pWatchPath, FSRoot root, uint32_t eventMask, Callback callback)
{
	pData = conf_new(FileSystem::Watcher::Data);
	pData->mWatchDir = FileSystem::FixPath(FileSystem::AddTrailingSlash(pWatchPath), root);
	uint32_t notifyFilter = 0;
	if (eventMask & EVENT_MODIFIED)
	{
		notifyFilter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
	}
	if (eventMask & EVENT_ACCESSED)
	{
		notifyFilter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
	}
	if (eventMask & (EVENT_DELETED | EVENT_CREATED))
	{
		notifyFilter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
	}

	pData->mNotifyFilter = notifyFilter;
	pData->hExitEvt = ::CreateEvent(NULL, TRUE, FALSE, NULL);
	pData->mCallback = callback;
	pData->mRun = TRUE;

	pData->mThreadDesc.pFunc = fswThreadFunc;
	pData->mThreadDesc.pData = pData;

	pData->mThread = create_thread(&pData->mThreadDesc);
}

FileSystem::Watcher::~Watcher()
{
	pData->mRun = FALSE;
	SetEvent(pData->hExitEvt);
	destroy_thread(pData->mThread);
	CloseHandle(pData->hExitEvt);
	conf_delete(pData);
}

#endif
