/////////////////////////////////////////////////////////////////////////////
//    WinMerge:  an interactive diff/merge utility
//    Copyright (C) 1997-2000  Thingamahoochie Software
//    Author: Dean Grimm
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
/////////////////////////////////////////////////////////////////////////////
/**
 *  @file Plugins.cpp
 *
 *  @brief Support for VBS Scriptlets, VB ActiveX DLL, VC++ COM DLL
 */ 
// ID line follows -- this is updated by SVN
// $Id: Plugins.cpp 7052 2009-12-22 17:45:22Z kimmov $

#include "Plugins.h"
#define POCO_NO_UNWINDOWS 1
#include <vector>
#include <list>
#include <algorithm>
#include <cstdarg>
#include <cassert>
#include <Poco/Mutex.h>
#include <Poco/ScopedLock.h>
#include <Poco/RegularExpression.h>
#include <windows.h>
#include "MergeApp.h"
#include "unicoder.h"
#include "FileFilterMgr.h"
#include "lwdisp.h"
#include "resource.h"
#include "Exceptions.h"
#include "RegKey.h"
#include "paths.h"
#include "Environment.h"
#include "FileFilter.h"

using std::vector;
using Poco::RegularExpression;
using Poco::FastMutex;
using Poco::ScopedLock;

/**
 * @brief Category of transformation : define the transformation events
 *
 * @note USER categories are calls to scriptlets, or VB ActiveX DLL, or VC COM DLL
 * Use text definition : if you add one, nothing to do ; 
 * if you change one, you just have change the dll/scripts for that event
 */
const wchar_t *TransformationCategories[] = 
{
	L"BUFFER_PREDIFF",
	L"FILE_PREDIFF",
	L"EDITOR_SCRIPT",
	L"BUFFER_PACK_UNPACK",
	L"FILE_PACK_UNPACK",
	L"FILE_FOLDER_PACK_UNPACK",
	NULL,		// last empty : necessary
};

static vector<String> theScriptletList;
/// Need to lock the *.sct so the user can't delete them
static vector<HANDLE> theScriptletHandleList;
static bool scriptletsLoaded=false;
static FastMutex scriptletsSem;

template<class T> struct AutoReleaser
{
	AutoReleaser(T *ptr) : p(ptr) {}
	~AutoReleaser() { if (p) p->Release(); }
	T *p;
};

////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Check for the presence of Windows Script
 *
 * .sct plugins require this optional component
 */
bool IsWindowsScriptThere()
{
	CRegKeyEx keyFile;
	if (!keyFile.QueryRegMachine(_T("SOFTWARE\\Classes\\scriptletfile\\AutoRegister")))
		return false;

	String filename = keyFile.ReadString(_T(""), _T("")).c_str();
	keyFile.Close();
	if (filename.empty())
		return false;

	return (paths_DoesPathExist(filename) == IS_EXISTING_FILE);
}

////////////////////////////////////////////////////////////////////////////////
// scriptlet/activeX support for function names

// list the function IDs and names in a script or activeX dll
int GetFunctionsFromScript(IDispatch *piDispatch, vector<String>& namesArray, vector<int>& IdArray, INVOKEKIND wantedKind)
{
	HRESULT hr;
	UINT iValidFunc = 0;
	if (piDispatch)
	{
		ITypeInfo *piTypeInfo=0;
		unsigned  iTInfo = 0; // 0 for type information of IDispatch itself
		LCID  lcid=0; // locale for localized method names (ignore if no localized names)
		if (SUCCEEDED(hr = piDispatch->GetTypeInfo(iTInfo, lcid, &piTypeInfo)))
		{
			TYPEATTR *pTypeAttr=0;
			if (SUCCEEDED(hr = piTypeInfo->GetTypeAttr(&pTypeAttr)))
			{
				// allocate arrays for the returned structures
				// the names array is NULL terminated
				namesArray.resize(pTypeAttr->cFuncs+1);
				IdArray.resize(pTypeAttr->cFuncs+1);

				UINT iMaxFunc = pTypeAttr->cFuncs - 1;
				for (UINT iFunc = 0 ; iFunc <= iMaxFunc ; ++iFunc)
				{
					UINT iFuncDesc = iMaxFunc - iFunc;
					FUNCDESC *pFuncDesc;
					if (SUCCEEDED(hr = piTypeInfo->GetFuncDesc(iFuncDesc, &pFuncDesc)))
					{
						// exclude properties
						// exclude IDispatch inherited methods
						if (pFuncDesc->invkind & wantedKind && !(pFuncDesc->wFuncFlags & 1))
						{
							BSTR bstrName;
							UINT cNames;
							if (SUCCEEDED(hr = piTypeInfo->GetNames(pFuncDesc->memid,
								&bstrName, 1, &cNames)))
							{
								IdArray[iValidFunc] = pFuncDesc->memid;
								namesArray[iValidFunc] = ucr::toTString(bstrName);
								iValidFunc ++;
							}
							SysFreeString(bstrName);
						}
						piTypeInfo->ReleaseFuncDesc(pFuncDesc);
					}
				}
				piTypeInfo->ReleaseTypeAttr(pTypeAttr);
			}
			piTypeInfo->Release();
		}
	}
	return iValidFunc;
}

int GetMethodsFromScript(IDispatch *piDispatch, vector<String>& namesArray, vector<int> &IdArray)
{
	return GetFunctionsFromScript(piDispatch, namesArray, IdArray, INVOKE_FUNC);
}
int GetPropertyGetsFromScript(IDispatch *piDispatch, vector<String>& namesArray, vector<int> &IdArray)
{
	return GetFunctionsFromScript(piDispatch, namesArray, IdArray, INVOKE_PROPERTYGET);
}


// search a function name in a scriptlet or activeX dll
bool SearchScriptForMethodName(LPDISPATCH piDispatch, const wchar_t *functionName)
{
	bool bFound = false;

	vector<String> namesArray;
	vector<int> IdArray;
	int nFnc = GetMethodsFromScript(piDispatch, namesArray, IdArray);

	String tfuncname = ucr::toTString(functionName);
	int iFnc;
	for (iFnc = 0 ; iFnc < nFnc ; iFnc++)
	{
		if (namesArray[iFnc] == tfuncname)
			bFound = true;
	}
	return bFound;
}

// search a property name (with get interface) in a scriptlet or activeX dll
bool SearchScriptForDefinedProperties(IDispatch *piDispatch, const wchar_t *functionName)
{
	bool bFound = false;

	vector<String> namesArray;
	vector<int> IdArray;
	int nFnc = GetPropertyGetsFromScript(piDispatch, namesArray, IdArray);

	String tfuncname = ucr::toTString(functionName);
	int iFnc;
	for (iFnc = 0 ; iFnc < nFnc ; iFnc++)
	{
		if (namesArray[iFnc] == tfuncname)
			bFound = true;
	}
	return bFound;
}


int CountMethodsInScript(LPDISPATCH piDispatch)
{
	vector<String> namesArray;
	vector<int> IdArray;
	int nFnc = GetMethodsFromScript(piDispatch, namesArray, IdArray);

	return nFnc;
}

/** 
 * @return ID of the function or -1 if no function with this index
 */
int GetMethodIDInScript(LPDISPATCH piDispatch, int methodIndex)
{
	int fncID;

	vector<String> namesArray;
	vector<int> IdArray;
	int nFnc = GetMethodsFromScript(piDispatch, namesArray, IdArray);

	if (methodIndex < nFnc)
	{
		fncID = IdArray[methodIndex];
	}
	else
	{
		fncID = -1;
	}
	
	return fncID;
}

////////////////////////////////////////////////////////////////////////////////
// find scripts/activeX for an event : each event is assigned to a subdirectory 


/**
 * @brief Get a list of scriptlet file
 *
 * @return Returns an array of LPSTR
 */
static void GetScriptletsAt(const TCHAR *szSearchPath, const TCHAR *extension, vector<String>& scriptlets )
{
	WIN32_FIND_DATA ffi;
	String strFileSpec = string_format(_T("%s*%s"), szSearchPath, extension);
	HANDLE hff = FindFirstFile(strFileSpec.c_str(), &ffi);
	
	if (  hff != INVALID_HANDLE_VALUE )
	{
		do
		{
			if (!(ffi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				strFileSpec = string_format(_T("%s%s"), szSearchPath, ffi.cFileName);
				scriptlets.push_back(strFileSpec);  
			}
		}
		while (FindNextFile(hff, &ffi));
		FindClose(hff);
	}
}

void PluginInfo::LoadFilterString()
{
	m_filters.clear();

	String sLine(m_filtersText);
	String sPiece;

	while(1)
	{
		String::size_type pos = sLine.rfind(';');
		sPiece = sLine.substr(pos+1);
		if (pos == String::npos)
			pos = 0;
		sLine = sLine.substr(0, pos);
		if (sPiece.empty())
			break;

		sPiece = string_makeupper(string_trim_ws_begin(sPiece));

		int re_opts = 0;
		std::string regexString = ucr::toUTF8(sPiece);
		re_opts |= RegularExpression::RE_UTF8;
		try
		{
			m_filters.push_back(FileFilterElementPtr(new FileFilterElement(regexString, re_opts)));
		}
		catch (...)
		{
			// TODO:
		}
	};
}


bool PluginInfo::TestAgainstRegList(const String& szTest) const
{
	if (m_filters.empty() || szTest.empty())
		return false;

	String sLine = szTest;
	String sPiece;

	while(!sLine.empty())
	{
		String::size_type pos = sLine.rfind('|');
		sPiece = sLine.substr(pos+1);
		if (pos == String::npos)
			pos = 0;
		sLine = sLine.substr(0, pos);
		if (sPiece.empty())
			continue;
		sPiece = string_makeupper(string_trim_ws_begin(sPiece));

		if (::TestAgainstRegList(&m_filters, sPiece))
			return true;
	};

	return false;
}

/**
 * @brief Log technical explanation, in English, of script error
 */
static void
ScriptletError(const String & scriptletFilepath, const wchar_t *transformationEvent, const TCHAR *szError)
{
	String msg = _T("Plugin scriptlet error <")
		+ scriptletFilepath
		+ _T("> [")
		+ ucr::toTString(transformationEvent)
		+ _T("] ")
		+ szError;
    LogErrorString(msg);
}

/**
 * @brief Tiny structure that remembers current scriptlet & event info for calling Log
 */
struct ScriptInfo
{
	ScriptInfo(const String & scriptletFilepath, const wchar_t *transformationEvent)
		: m_scriptletFilepath(scriptletFilepath)
		, m_transformationEvent(transformationEvent)
	{
	}
	void Log(const TCHAR *szError)
	{
		ScriptletError(m_scriptletFilepath, m_transformationEvent, szError);
	}
	const String & m_scriptletFilepath;
	const wchar_t *m_transformationEvent;
};

/**
 * @brief Try to load a plugin
 *
 * @return 1 if plugin handles this event, 0 if not, negatives for errors
 */
int PluginInfo::LoadPlugin(const String & scriptletFilepath, const wchar_t *transformationEvent)
{
	// set up object in case we need to log info
	ScriptInfo scinfo(scriptletFilepath, transformationEvent);

	// Search for the class "WinMergeScript"
	LPDISPATCH lpDispatch = CreateDispatchBySource(scriptletFilepath.c_str(), L"WinMergeScript");
	if (lpDispatch == 0)
	{
		scinfo.Log(_T("WinMergeScript entry point not found"));
		return -10; // error
	}

	// Ensure that interface is released if any bad exit or exception
	AutoReleaser<IDispatch> drv(lpDispatch);

	// Is this plugin for this transformationEvent ?
	VARIANT ret;
	// invoke mandatory method get PluginEvent
	VariantInit(&ret);
	if (!SearchScriptForDefinedProperties(lpDispatch, L"PluginEvent"))
	{
		scinfo.Log(_T("PluginEvent method missing"));
		return -20; // error
	}
	HRESULT h = ::invokeW(lpDispatch, &ret, L"PluginEvent", opGet[0], NULL);
	if (FAILED(h) || ret.vt != VT_BSTR)
	{
		scinfo.Log(	_T("Error accessing PluginEvent method"));
		return -30; // error
	}
	if (wcscmp(ret.bstrVal, transformationEvent) != 0)
	{
		return 0; // doesn't handle this event
	}
	VariantClear(&ret);

	// plugins PREDIFF or PACK_UNPACK : functions names are mandatory
	// Check that the plugin offers the requested functions
	// set the mode for the events which uses it
	bool bFound = true;
	if (wcscmp(transformationEvent, L"BUFFER_PREDIFF") == 0)
	{
		bFound &= SearchScriptForMethodName(lpDispatch, L"PrediffBufferW");
	}
	else if (wcscmp(transformationEvent, L"FILE_PREDIFF") == 0)
	{
		bFound &= SearchScriptForMethodName(lpDispatch, L"PrediffFile");
	}
	else if (wcscmp(transformationEvent, L"BUFFER_PACK_UNPACK") == 0)
	{
		bFound &= SearchScriptForMethodName(lpDispatch, L"UnpackBufferA");
		bFound &= SearchScriptForMethodName(lpDispatch, L"PackBufferA");
	}
	else if (wcscmp(transformationEvent, L"FILE_PACK_UNPACK") == 0)
	{
		bFound &= SearchScriptForMethodName(lpDispatch, L"UnpackFile");
		bFound &= SearchScriptForMethodName(lpDispatch, L"PackFile");
	}
	else if (wcscmp(transformationEvent, L"FILE_FOLDER_PACK_UNPACK") == 0)
	{
		bFound &= SearchScriptForMethodName(lpDispatch, L"IsFolder");
		bFound &= SearchScriptForMethodName(lpDispatch, L"UnpackFile");
		bFound &= SearchScriptForMethodName(lpDispatch, L"PackFile");
		bFound &= SearchScriptForMethodName(lpDispatch, L"UnpackFolder");
		bFound &= SearchScriptForMethodName(lpDispatch, L"PackFolder");
	}
	if (!bFound)
	{
		// error (Plugin doesn't support the method as it claimed)
		scinfo.Log(_T("Plugin doesn't support the method as it claimed"));
		return -40; 
	}

	// plugins EDITOR_SCRIPT : functions names are free
	// there may be several functions inside one script, count the number of functions
	if (wcscmp(transformationEvent, L"EDITOR_SCRIPT") == 0)
	{
		m_nFreeFunctions = CountMethodsInScript(lpDispatch);
		if (m_nFreeFunctions == 0)
			// error (Plugin doesn't offer any method, what is this ?)
			return -50;
	}


	// get optional property PluginDescription
	if (SearchScriptForDefinedProperties(lpDispatch, L"PluginDescription"))
	{
		h = ::invokeW(lpDispatch, &ret, L"PluginDescription", opGet[0], NULL);
		if (FAILED(h) || ret.vt != VT_BSTR)
		{
			scinfo.Log(_T("Plugin had PluginDescription property, but error getting its value"));
			return -60; // error (Plugin had PluginDescription property, but error getting its value)
		}
		m_description = ucr::toTString(ret.bstrVal);
	}
	else
	{
		// no description, use filename
		m_description = paths_FindFileName(scriptletFilepath);
	}
	VariantClear(&ret);

	// get PluginFileFilters
	bool hasPluginFileFilters = false;
	if (SearchScriptForDefinedProperties(lpDispatch, L"PluginFileFilters"))
	{
		h = ::invokeW(lpDispatch, &ret, L"PluginFileFilters", opGet[0], NULL);
		if (FAILED(h) || ret.vt != VT_BSTR)
		{
			scinfo.Log(_T("Plugin had PluginFileFilters property, but error getting its value"));
			return -70; // error (Plugin had PluginFileFilters property, but error getting its value)
		}
		m_filtersText = ucr::toTString(ret.bstrVal);
		hasPluginFileFilters = true;
	}
	else
	{
		m_bAutomatic = false;
		m_filtersText = _T(".");
	}
	VariantClear(&ret);

	// get optional property PluginIsAutomatic
	if (SearchScriptForDefinedProperties(lpDispatch, L"PluginIsAutomatic"))
	{
		h = ::invokeW(lpDispatch, &ret, L"PluginIsAutomatic", opGet[0], NULL);
		if (FAILED(h) || ret.vt != VT_BOOL)
		{
			scinfo.Log(_T("Plugin had PluginIsAutomatic property, but error getting its value"));
			return -80; // error (Plugin had PluginIsAutomatic property, but error getting its value)
		}
		m_bAutomatic = !!ret.boolVal;
	}
	else
	{
		if (hasPluginFileFilters)
		{
			scinfo.Log(_T("Plugin had PluginFileFilters property, but lacked PluginIsAutomatic property"));
			// PluginIsAutomatic property is mandatory for Plugins with PluginFileFilters property
			return -90;
		}
		// default to false when Plugin doesn't have property
		m_bAutomatic = false;
	}
	VariantClear(&ret);

	LoadFilterString();

	// keep the filename
	m_name = paths_FindFileName(scriptletFilepath);

	// Clear the autorelease holder
	drv.p = NULL;

	m_lpDispatch = lpDispatch;

	m_filepath = scriptletFilepath;

	return 1;
}

static void ReportPluginLoadFailure(const String & scriptletFilepath, const wchar_t *transformationEvent)
{
	String sEvent = ucr::toTString(transformationEvent);
	AppErrorMessageBox(string_format(_T("Exception loading plugin for event: %s\r\n%s"), sEvent.c_str(), scriptletFilepath.c_str()));
}

/**
 * @brief Guard call to LoadPlugin with Windows SEH to trap GPFs
 *
 * @return same as LoadPlugin (1=has event, 0=doesn't have event, errors are negative)
 */
static int LoadPluginWrapper(PluginInfo & plugin, const String & scriptletFilepath, const wchar_t *transformationEvent)
{
	SE_Handler seh;
	try
	{
		return plugin.LoadPlugin(scriptletFilepath, transformationEvent);
	}
	catch (SE_Exception&)
	{
		ReportPluginLoadFailure(scriptletFilepath, transformationEvent);
	}
	return false;
}

/**
 * @brief Return list of all candidate plugins in module path
 *
 * Computes list only the first time, and caches it.
 * Lock the plugins *.sct (.ocx and .dll are locked when the interface is created)
 */
static vector<String>& LoadTheScriptletList()
{
	FastMutex::ScopedLock lock(scriptletsSem);
	if (!scriptletsLoaded)
	{
		String path = env_GetProgPath() + _T("\\MergePlugins\\");

		if (IsWindowsScriptThere())
			GetScriptletsAt(path.c_str(), _T(".sct"), theScriptletList );		// VBS/JVS scriptlet
		else
			LogErrorString(_T("\n  .sct plugins disabled (Windows Script Host not found)"));
		GetScriptletsAt(path.c_str(), _T(".ocx"), theScriptletList );		// VB COM object
		GetScriptletsAt(path.c_str(), _T(".dll"), theScriptletList );		// VC++ COM object
		scriptletsLoaded = true;

		// lock the *.sct to avoid them being deleted/moved away
		int i;
		for (i = 0 ; i < theScriptletList.size() ; i++)
		{
			String scriptlet = theScriptletList.at(i);
			if (scriptlet.length() > 4 && string_compare_nocase(scriptlet.substr(scriptlet.length() - 4), _T(".sct")) != 0)
			{
				// don't need to lock this file
				theScriptletHandleList.push_back(NULL);
				continue;
			}

			HANDLE hFile;
			hFile=CreateFile(scriptlet.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
				0, NULL);
			if (hFile == INVALID_HANDLE_VALUE)
			{
				theScriptletList.erase(theScriptletList.begin() + i);
				i --;
			}
			else
			{
				theScriptletHandleList.push_back(hFile);
			}
		}
	}
	return theScriptletList;
}
/**
 * @brief Delete the scriptlet list and delete locks to *.sct
 *
 * Allow to load it again
 */
static void UnloadTheScriptletList()
{
	FastMutex::ScopedLock lock(scriptletsSem);
	if (scriptletsLoaded)
	{
		int i;
		for (i = 0 ; i < theScriptletHandleList.size() ; i++)
		{
			HANDLE hFile = theScriptletHandleList.at(i);
			if (hFile != 0)
				CloseHandle(hFile);
		}

		theScriptletHandleList.clear();
		theScriptletList.clear();
		scriptletsLoaded = false;
	}
}

/**
 * @brief Remove a candidate plugin from the cache
 */
static void RemoveScriptletCandidate(const String &scriptletFilepath)
{
	for (int i=0; i<theScriptletList.size(); ++i)
	{
		if (scriptletFilepath == theScriptletList[i])
		{
			HANDLE hFile = theScriptletHandleList.at(i);
			if (hFile != 0)
				CloseHandle(hFile);

			theScriptletHandleList.erase(theScriptletHandleList.begin() + i);
			theScriptletList.erase(theScriptletList.begin() + i);
			return;
		}
	}
}

/** 
 * @brief Get available scriptlets for an event
 *
 * @return Returns an array of valid LPDISPATCH
 */
static PluginArray * GetAvailableScripts( const wchar_t *transformationEvent, bool getScriptletsToo ) 
{
	vector<String>& scriptlets = LoadTheScriptletList();

	PluginArray * pPlugins = new PluginArray;

	int i;
	std::list<String> badScriptlets;
	for (i = 0 ; i < scriptlets.size() ; i++)
	{
		// Note all the info about the plugin
		PluginInfoPtr plugin(new PluginInfo);

		String scriptletFilepath = scriptlets.at(i);
		int rtn = LoadPluginWrapper(*plugin.get(), scriptletFilepath, transformationEvent);
		if (rtn == 1)
		{
			// Plugin has this event
			pPlugins->push_back(plugin);
		}
		else if (rtn < 0)
		{
			// Plugin is bad
			badScriptlets.push_back(scriptletFilepath);
		}
	}

	// Remove any bad plugins from the cache
	// This might be a good time to see if the user wants to abort or continue
	while (!badScriptlets.empty())
	{
		RemoveScriptletCandidate(badScriptlets.front());
		badScriptlets.pop_front();
	}

	return pPlugins;
}

static void FreeAllScripts(PluginArrayPtr& pArray) 
{
	pArray->clear();
	pArray.reset();
}

////////////////////////////////////////////////////////////////////////////////////
// class CScriptsOfThread : cache the interfaces during the thread life

CScriptsOfThread::CScriptsOfThread()
{
	// count number of events
	int i;
	for (i = 0 ;  ; i ++)
		if (TransformationCategories[i] == NULL)
			break;
	nTransformationEvents = i;

	// initialize the thread data
	m_nThreadId = GetCurrentThreadId();
	m_nLocks = 0;
	// initialize the plugins pointers
	typedef PluginArray * LPPluginArray;
	m_aPluginsByEvent.resize(nTransformationEvents);
	// CoInitialize the thread, keep the returned value for the destructor 
	hrInitialize = CoInitialize(NULL);
	assert(hrInitialize == S_OK || hrInitialize == S_FALSE);
}

CScriptsOfThread::~CScriptsOfThread()
{
	if (hrInitialize == S_OK || hrInitialize == S_FALSE)
		CoUninitialize();

	FreeAllScripts();
}

bool CScriptsOfThread::bInMainThread()
{
	return (CAllThreadsScripts::bInMainThread(this));
}

PluginArray * CScriptsOfThread::GetAvailableScripts(const wchar_t *transformationEvent)
{
	int i;
	for (i = 0 ; i < nTransformationEvents ; i ++)
		if (wcscmp(transformationEvent, TransformationCategories[i]) == 0)
		{
			if (m_aPluginsByEvent[i] == NULL)
				m_aPluginsByEvent[i].reset(::GetAvailableScripts(transformationEvent, bInMainThread()));
			return m_aPluginsByEvent[i].get();
		}
	// return a pointer to an empty list
	static PluginArray noPlugin;
	return &noPlugin;
}



void CScriptsOfThread::FreeAllScripts()
{
	// release all the scripts of the thread
	int i;
	for (i = 0 ; i < nTransformationEvents ; i++)
		if (m_aPluginsByEvent[i])
			::FreeAllScripts(m_aPluginsByEvent[i]);

	// force to reload the scriptlet list
	UnloadTheScriptletList();
}

void CScriptsOfThread::FreeScriptsForEvent(const wchar_t *transformationEvent)
{
	int i;
	for (i = 0 ; i < nTransformationEvents ; i ++)
		if (wcscmp(transformationEvent, TransformationCategories[i]) == 0)
		{
			if (m_aPluginsByEvent[i])
				::FreeAllScripts(m_aPluginsByEvent[i]);
			return;
		}
}

PluginInfo *CScriptsOfThread::GetAutomaticPluginByFilter(const wchar_t *transformationEvent, const String& filteredText)
{
	PluginArray * piFileScriptArray = GetAvailableScripts(transformationEvent);
	for (int step = 0 ; step < piFileScriptArray->size() ; step ++)
	{
		const PluginInfoPtr & plugin = piFileScriptArray->at(step);
		if (plugin->m_bAutomatic == false)
			continue;
		if (plugin->TestAgainstRegList(filteredText) == false)
			continue;
		return plugin.get();
	}
	return NULL;
}

PluginInfo * CScriptsOfThread::GetPluginByName(const wchar_t *transformationEvent, const String& name)
{
	int i;
	for (i = 0 ; i < nTransformationEvents ; i ++)
		if (wcscmp(transformationEvent, TransformationCategories[i]) == 0)
		{
			if (m_aPluginsByEvent[i] == NULL)
				m_aPluginsByEvent[i].reset(::GetAvailableScripts(transformationEvent, bInMainThread()));

			for (int j = 0 ; j <  m_aPluginsByEvent[i]->size() ; j++)
				if (m_aPluginsByEvent[i]->at(j)->m_name == name)
					return m_aPluginsByEvent[i]->at(j).get();
		}
	return NULL;
}

PluginInfo *  CScriptsOfThread::GetPluginInfo(LPDISPATCH piScript)
{
	int i, j;
	for (i = 0 ; i < nTransformationEvents ; i ++) 
	{
		if (m_aPluginsByEvent[i] == NULL)
			continue;
		const PluginArrayPtr& pArray = m_aPluginsByEvent[i];
		for (j = 0 ; j < pArray->size() ; j++)
			if ((*pArray)[j]->m_lpDispatch == piScript)
				return (*pArray)[j].get();
	}

	return NULL;
}

////////////////////////////////////////////////////////////////////////////////////
// class CAllThreadsScripts : array of CScriptsOfThread, one per active thread

std::vector<CScriptsOfThread *> CAllThreadsScripts::m_aAvailableThreads;
FastMutex m_aAvailableThreadsLock;

void CAllThreadsScripts::Add(CScriptsOfThread * scripts)
{
	FastMutex::ScopedLock lock(m_aAvailableThreadsLock);
	// add the thread in the array

	// register in the array
	m_aAvailableThreads.push_back(scripts);
}

void CAllThreadsScripts::Remove(CScriptsOfThread * scripts)
{
	FastMutex::ScopedLock lock(m_aAvailableThreadsLock);
	// unregister from the list
	std::vector<CScriptsOfThread *>::iterator it;
	for (it =  m_aAvailableThreads.begin(); it != m_aAvailableThreads.end(); ++it)
		if ((*it) == scripts)
		{
			m_aAvailableThreads.erase(it);
			break;
		}
}

CScriptsOfThread * CAllThreadsScripts::GetActiveSet()
{
	FastMutex::ScopedLock lock(m_aAvailableThreadsLock);
	unsigned long nThreadId = GetCurrentThreadId();
	int i;
	for (i = 0 ; i < m_aAvailableThreads.size() ; i++)
		if (m_aAvailableThreads[i] && m_aAvailableThreads[i]->m_nThreadId == nThreadId)
			return m_aAvailableThreads[i];
	assert(0);
	return NULL;
}
CScriptsOfThread * CAllThreadsScripts::GetActiveSetNoAssert()
{
	FastMutex::ScopedLock lock(m_aAvailableThreadsLock);
	unsigned long nThreadId = GetCurrentThreadId();
	int i;
	for (i = 0 ; i < m_aAvailableThreads.size() ; i++)
		if (m_aAvailableThreads[i] && m_aAvailableThreads[i]->m_nThreadId == nThreadId)
			return m_aAvailableThreads[i];
	return NULL;
}

bool CAllThreadsScripts::bInMainThread(CScriptsOfThread * scripts)
{
	FastMutex::ScopedLock lock(m_aAvailableThreadsLock);
	return (scripts == m_aAvailableThreads[0]);
}

////////////////////////////////////////////////////////////////////////////////////
// class CAssureScriptsForThread : control creation/destruction of CScriptsOfThread

CAssureScriptsForThread::CAssureScriptsForThread()
{
	CScriptsOfThread * scripts = CAllThreadsScripts::GetActiveSetNoAssert();
	if (scripts == NULL)
	{
		scripts = new CScriptsOfThread;
		// insert the script in the repository
		CAllThreadsScripts::Add(scripts);
	}
	scripts->Lock();
}
CAssureScriptsForThread::~CAssureScriptsForThread()
{
	CScriptsOfThread * scripts = CAllThreadsScripts::GetActiveSetNoAssert();
	if (scripts == NULL)
		return;
	if (scripts->Unlock() == true)
	{
		CAllThreadsScripts::Remove(scripts);
		delete scripts;
	}
}

////////////////////////////////////////////////////////////////////////////////
// reallocation, take care of flag bWriteable

static void reallocBuffer(LPSTR & pszBuf, UINT & nOldSize, UINT nSize, bool bWriteable)
{
	if (!bWriteable)
		// alloc a new buffer
		pszBuf = (LPSTR) malloc(nSize);
	else if (nSize > nOldSize) 
	{
		// free the previous buffer, alloc a new one (so we don't copy the old values)
		free(pszBuf);
		pszBuf = (LPSTR) malloc(nSize);
	}
	else
		// just truncate the buffer
		pszBuf = (LPSTR) realloc(pszBuf, nSize);
	nOldSize = nSize;
}
static void reallocBuffer(LPWSTR & pszBuf, UINT & nOldSize, UINT nSize, bool bWriteable)
{
	if (!bWriteable)
		// alloc a new buffer
		pszBuf = (LPWSTR) malloc(nSize*sizeof(WCHAR));
	else if (nSize > nOldSize) 
	{
		// free the previous buffer, alloc a new one (so we don't copy the old values)
		free(pszBuf);
		pszBuf = (LPWSTR) malloc(nSize*sizeof(WCHAR));
	}
	else
		// just truncate the buffer
		pszBuf = (LPWSTR) realloc(pszBuf, nSize*sizeof(WCHAR));
	nOldSize = nSize;
}


////////////////////////////////////////////////////////////////////////////////
// wrap invokes with error handlers

/**
 * @brief Display a message box with the plugin name and the error message
 *
 * @note Use MessageBox instead of AfxMessageBox so we can set the caption.
 * VB/VBS plugins has an internal error handler, and a message box with caption,
 * and we try to reproduce it for other plugins.
 */
static void ShowPluginErrorMessage(IDispatch *piScript, LPTSTR description)
{
	PluginInfo * pInfo = CAllThreadsScripts::GetActiveSet()->GetPluginInfo(piScript);
	assert(pInfo != NULL);
	assert(description != NULL);	
	AppErrorMessageBox(string_format(_T("%s: %s"), pInfo->m_name.c_str(), description));
}

/**
 * @brief safe invoke helper (by ordinal)
 *
 * @note Free all variants passed to it (except ByRef ones) 
 */
static HRESULT safeInvokeA(LPDISPATCH pi, VARIANT *ret, DISPID id, LPCCH op, ...)
{
	HRESULT h;
	SE_Handler seh;
	TCHAR errorText[500];
	bool bExceptionCatched = false;	
#ifdef WIN64
	int nargs = LOBYTE((UINT_PTR)op);
	vector<VARIANT> args(nargs);
	va_list list;
	va_start(list, op);
	for (vector<VARIANT>::iterator it = args.begin(); it != args.end(); ++it)
		*it = va_arg(list, VARIANT);
	va_end(list);
#endif

	try 
	{
#ifdef WIN64
		h = invokeA(pi, ret, id, op, nargs == 0 ? NULL : &args[0]);
#else
		h = invokeA(pi, ret, id, op, (VARIANT *)(&op + 1));
#endif
	}
	catch(SE_Exception& e) 
	{
		// structured exception are catched here thanks to class SE_Exception
		if (!(e.GetErrorMessage(errorText, 500, NULL)))
			// don't localize this as we do not localize the known exceptions
			_tcscpy(errorText, _T("Unknown CException"));
		bExceptionCatched = true;
	}
	catch(...) 
	{
		// don't localize this as we do not localize the known exceptions
		_tcscpy(errorText, _T("Unknown C++ exception"));
		bExceptionCatched = true;
	}

	if (bExceptionCatched)
	{
		ShowPluginErrorMessage(pi, errorText);
		// set h to FAILED
		h = -1;
	}

	return h;
}
/**
 * @brief safe invoke helper (by function name)
 *
 * @note Free all variants passed to it (except ByRef ones) 
 */
static HRESULT safeInvokeW(LPDISPATCH pi, VARIANT *ret, LPCOLESTR silent, LPCCH op, ...)
{
	HRESULT h;
	SE_Handler seh;
	TCHAR errorText[500];
	bool bExceptionCatched = false;
#ifdef WIN64
	int nargs = LOBYTE((UINT_PTR)op);
	vector<VARIANT> args(nargs);
	va_list list;
	va_start(list, op);
	for (vector<VARIANT>::iterator it = args.begin(); it != args.end(); ++it)
		*it = va_arg(list, VARIANT);
	va_end(list);
#endif
	
	try 
	{
#ifdef WIN64
		h = invokeW(pi, ret, silent, op, nargs == 0 ? NULL : &args[0]);
#else
		h = invokeW(pi, ret, silent, op, (VARIANT *)(&op + 1));
#endif
	}
	catch(SE_Exception& e) 
	{
		// structured exception are catched here thanks to class SE_Exception
		if (!(e.GetErrorMessage(errorText, 500, NULL)))
			// don't localize this as we do not localize the known exceptions
			_tcscpy(errorText, _T("Unknown CException"));
		bExceptionCatched = true;
	}
	catch(...) 
	{
		// don't localize this as we do not localize the known exceptions
		_tcscpy(errorText, _T("Unknown C++ exception"));
		bExceptionCatched = true;
	}

	if (bExceptionCatched)
	{
		ShowPluginErrorMessage(pi, errorText);
		// set h to FAILED
		h = -1;
	}

	return h;
}

////////////////////////////////////////////////////////////////////////////////
// invoke for plugins

/*
 * ----- about VariantClear -----
 * VariantClear is done in safeInvokeW/safeInvokeA except for :
 * - the returned value
 * - BYREF arguments
 * note : BYREF arguments don't need VariantClear if the refered value
 * is deleted in the function destructor. Example :
 * {
 *   int Value;
 *   VARIANT vValue;
 *   vValue.plVal = &vValue;
 *   ...
 */

bool InvokePrediffBuffer(BSTR & bstrBuf, int & nChanged, IDispatch *piScript)
{
	UINT nBufSize = SysStringLen(bstrBuf);

	// prepare the arguments
	// argument text buffer by reference
	VARIANT vpbstrBuf;
	vpbstrBuf.vt = VT_BYREF | VT_BSTR;
	vpbstrBuf.pbstrVal = &bstrBuf;
	// argument buffer size by reference
	VARIANT vpiSize;
	vpiSize.vt = VT_BYREF | VT_I4;
	vpiSize.plVal = (long*) &nBufSize;
	// argument flag changed (VT_BOOL is short)
	VARIANT_BOOL changed = 0;
	VARIANT vpboolChanged;
	vpboolChanged.vt = VT_BYREF | VT_BOOL;
	vpboolChanged.pboolVal = &changed;
	// argument return value (VT_BOOL is short)
	VARIANT vboolHandled;
	vboolHandled.vt = VT_BOOL;
	vboolHandled.boolVal = false;

	// invoke method by name, reverse order for arguments
	// for VC, if the invoked function changes the buffer address, 
	// it must free the old buffer with SysFreeString
	// VB does it automatically
	// VARIANT_BOOL DiffingPreprocessW(BSTR * buffer, UINT * nSize, VARIANT_BOOL * bChanged)
	HRESULT h = ::safeInvokeW(piScript,	&vboolHandled, L"PrediffBufferW", opFxn[3], 
                            vpboolChanged, vpiSize, vpbstrBuf);
	bool bSuccess = ! FAILED(h) && vboolHandled.boolVal;
	if (bSuccess && changed)
	{
		// remove trailing charracters in the rare case that bstrBuf was not resized 
		if (SysStringLen(bstrBuf) != nBufSize)
			bSuccess = !FAILED(SysReAllocStringLen(&bstrBuf, bstrBuf, nBufSize));
		if (bSuccess)
			nChanged ++;
	}

	// clear the returned variant
	VariantClear(&vboolHandled);

	return 	(bSuccess);
}

bool InvokeUnpackBuffer(VARIANT & array, int & nChanged, IDispatch *piScript, int & subcode)
{
	LONG nArraySize;
	SafeArrayGetUBound(array.parray, 0, &nArraySize);
	++nArraySize;

	// prepare the arguments
	// argument file buffer
	VARIANT vparrayBuf;
	vparrayBuf.vt = VT_BYREF | VT_ARRAY | VT_UI1;
	vparrayBuf.pparray = &(array.parray);
	// argument buffer size by reference
	VARIANT vpiSize;
	vpiSize.vt = VT_BYREF | VT_I4;
	vpiSize.plVal = (long*) &nArraySize;
	// argument flag changed (VT_BOOL is short)
	VARIANT_BOOL changed = 0;
	VARIANT vpboolChanged;
	vpboolChanged.vt = VT_BYREF | VT_BOOL;
	vpboolChanged.pboolVal = &changed;
	// argument subcode by reference
	VARIANT viSubcode;
	viSubcode.vt = VT_BYREF | VT_I4;
	viSubcode.plVal = (long*) &subcode;
	// argument return value (VT_BOOL is short)
	VARIANT vboolHandled;
	vboolHandled.vt = VT_BOOL;
	vboolHandled.boolVal = false;

	// invoke method by name, reverse order for arguments
	// VARIANT_BOOL UnpackBufferA(SAFEARRAY * array, UINT * nSize, VARIANT_BOOL * bChanged, UINT * subcode)
	HRESULT h = ::safeInvokeW(piScript,	&vboolHandled, L"UnpackBufferA", opFxn[4], 
                            viSubcode, vpboolChanged, vpiSize, vparrayBuf);
	bool bSuccess = ! FAILED(h) && vboolHandled.boolVal;
	if (bSuccess && changed)
	{
		// remove trailing charracters if the array was not resized
		LONG nNewArraySize;
		SafeArrayGetUBound(array.parray, 0, &nNewArraySize);
		++nNewArraySize;

		if (nNewArraySize != nArraySize)
		{
			SAFEARRAYBOUND sab = {nArraySize, 0};
			SafeArrayRedim(array.parray, &sab);
		}
		nChanged ++;
	}

	// clear the returned variant
	VariantClear(&vboolHandled);

	return 	(bSuccess);
}

bool InvokePackBuffer(VARIANT & array, int & nChanged, IDispatch *piScript, int subcode)
{
	LONG nArraySize;
	SafeArrayGetUBound(array.parray, 0, &nArraySize);
	++nArraySize;

	// prepare the arguments
	// argument file buffer
	VARIANT vparrayBuf;
	vparrayBuf.vt = VT_BYREF | VT_ARRAY | VT_UI1;
	vparrayBuf.pparray = &(array.parray);
	// argument buffer size by reference
	VARIANT vpiSize;
	vpiSize.vt = VT_BYREF | VT_I4;
	vpiSize.plVal = (long*) &nArraySize;
	// argument flag changed (VT_BOOL is short)
	VARIANT_BOOL changed = 0;
	VARIANT vpboolChanged;
	vpboolChanged.vt = VT_BYREF | VT_BOOL;
	vpboolChanged.pboolVal = &changed;
	// argument subcode
	VARIANT viSubcode;
	viSubcode.vt = VT_I4;
	viSubcode.lVal = subcode;
	// argument return value (VT_BOOL is short)
	VARIANT vboolHandled;
	vboolHandled.vt = VT_BOOL;
	vboolHandled.boolVal = false;

	// invoke method by name, reverse order for arguments
	// VARIANT_BOOL PackBufferA(SAFEARRAY * array, UINT * nSize, VARIANT_BOOL * bChanged, UINT subcode)
	HRESULT h = ::safeInvokeW(piScript,	&vboolHandled, L"PackBufferA", opFxn[4], 
                            viSubcode, vpboolChanged, vpiSize, vparrayBuf);
	bool bSuccess = ! FAILED(h) && vboolHandled.boolVal;
	if (bSuccess && changed)
	{
		// remove trailing charracters if the array was not resized
		LONG nNewArraySize;
		SafeArrayGetUBound(array.parray, 0, &nNewArraySize);
		++nNewArraySize;

		if (nNewArraySize != nArraySize)
		{
			SAFEARRAYBOUND sab = {nArraySize, 0};
			SafeArrayRedim(array.parray, &sab);
		}
	}

	// clear the returned variant
	VariantClear(&vboolHandled);

	return 	(bSuccess);
}


static bool unpack(const wchar_t *method, const String& source, const String& dest, int & nChanged, IDispatch *piScript, int & subCode)
{
	// argument text  
	VARIANT vbstrSrc;
	vbstrSrc.vt = VT_BSTR;
	vbstrSrc.bstrVal = SysAllocString(ucr::toUTF16(source).c_str());
	// argument transformed text 
	VARIANT vbstrDst;
	vbstrDst.vt = VT_BSTR;
	vbstrDst.bstrVal = SysAllocString(ucr::toUTF16(dest).c_str());
	// argument subcode by reference
	VARIANT vpiSubcode;
	vpiSubcode.vt = VT_BYREF | VT_I4;
	vpiSubcode.plVal = (long*) &subCode;
	// argument flag changed (VT_BOOL is short)
	VARIANT_BOOL changed = 0;
	VARIANT vpboolChanged;
	vpboolChanged.vt = VT_BYREF | VT_BOOL;
	vpboolChanged.pboolVal = &changed;
	// argument return value (VT_BOOL is short)
	VARIANT vboolHandled;
	vboolHandled.vt = VT_BOOL;
	vboolHandled.boolVal = false;

	// invoke method by name, reverse order for arguments
	// VARIANT_BOOL UnpackFile(BSTR fileSrc, BSTR fileDst, VARIANT_BOOL * bChanged, INT * bSubcode)
	HRESULT h = ::safeInvokeW(piScript,	&vboolHandled, method, opFxn[4], 
	                          vpiSubcode, vpboolChanged, vbstrDst, vbstrSrc);
	bool bSuccess = ! FAILED(h) && vboolHandled.boolVal;
	if (bSuccess && changed)
		nChanged ++;

	// clear the returned variant
	VariantClear(&vboolHandled);

	return 	(bSuccess);
}

static bool pack(const wchar_t *method, const String& source, const String& dest, int & nChanged, IDispatch *piScript, int & subCode)
{
	// argument text  
	VARIANT vbstrSrc;
	vbstrSrc.vt = VT_BSTR;
	vbstrSrc.bstrVal = SysAllocString(ucr::toUTF16(source).c_str());
	// argument transformed text 
	VARIANT vbstrDst;
	vbstrDst.vt = VT_BSTR;
	vbstrDst.bstrVal = SysAllocString(ucr::toUTF16(dest).c_str());
	// argument subcode
	VARIANT viSubcode;
	viSubcode.vt = VT_I4;
	viSubcode.lVal = subCode;
	// argument flag changed (VT_BOOL is short)
	VARIANT_BOOL changed = 0;
	VARIANT vpboolChanged;
	vpboolChanged.vt = VT_BYREF | VT_BOOL;
	vpboolChanged.pboolVal = &changed;
	// argument return value (VT_BOOL is short)
	VARIANT vboolHandled;
	vboolHandled.vt = VT_BOOL;
	vboolHandled.boolVal = false;

	// invoke method by name, reverse order for arguments
	// VARIANT_BOOL PackFile(BSTR fileSrc, BSTR fileDst, VARIANT_BOOL * bChanged, INT bSubcode)
	HRESULT h = ::safeInvokeW(piScript,	&vboolHandled, method, opFxn[4], 
	                          viSubcode, vpboolChanged, vbstrDst, vbstrSrc);
	bool bSuccess = ! FAILED(h) && vboolHandled.boolVal;
	if (bSuccess && changed)
		nChanged ++;

	// clear the returned variant
	VariantClear(&vboolHandled);

	return 	(bSuccess);
}

bool InvokeUnpackFile(const String& fileSource, const String& fileDest, int & nChanged, IDispatch *piScript, int & subCode)
{
	return unpack(L"UnpackFile", fileSource, fileDest, nChanged, piScript, subCode);
}

bool InvokePackFile(const String& fileSource, const String& fileDest, int & nChanged, IDispatch *piScript, int subCode)
{
	return pack(L"PackFile", fileSource, fileDest, nChanged, piScript, subCode);
}

bool InvokeUnpackFolder(const String& fileSource, const String& folderDest, int & nChanged, IDispatch *piScript, int & subCode)
{
	return unpack(L"UnpackFolder", fileSource, folderDest, nChanged, piScript, subCode);
}

bool InvokePackFolder(const String& folderSource, const String& fileDest, int & nChanged, IDispatch *piScript, int subCode)
{
	return pack(L"PackFolder", folderSource, fileDest, nChanged, piScript, subCode);
}

bool InvokePrediffFile(const String& fileSource, const String& fileDest, int & nChanged, IDispatch *piScript)
{
	// argument text  
	VARIANT vbstrSrc;
	vbstrSrc.vt = VT_BSTR;
	vbstrSrc.bstrVal = SysAllocString(ucr::toUTF16(fileSource).c_str());
	// argument transformed text 
	VARIANT vbstrDst;
	vbstrDst.vt = VT_BSTR;
	vbstrDst.bstrVal = SysAllocString(ucr::toUTF16(fileDest).c_str());
	// argument flag changed (VT_BOOL is short)
	VARIANT_BOOL changed = 0;
	VARIANT vpboolChanged;
	vpboolChanged.vt = VT_BYREF | VT_BOOL;
	vpboolChanged.pboolVal = &changed;
	// argument return value (VT_BOOL is short)
	VARIANT vboolHandled;
	vboolHandled.vt = VT_BOOL;
	vboolHandled.boolVal = false;

	// invoke method by name, reverse order for arguments
	// VARIANT_BOOL PrediffFile(BSTR fileSrc, BSTR fileDst, VARIANT_BOOL * bChanged)
	HRESULT h = ::safeInvokeW(piScript,	&vboolHandled, L"PrediffFile", opFxn[3], 
                            vpboolChanged, vbstrDst, vbstrSrc);
	bool bSuccess = ! FAILED(h) && vboolHandled.boolVal;
	if (bSuccess && changed)
		nChanged ++;

	// clear the returned variant
	VariantClear(&vboolHandled);

	return 	(bSuccess);
}


bool InvokeTransformText(String & text, int & changed, IDispatch *piScript, int fncId)
{
	// argument text  
	VARIANT pvPszBuf;
	pvPszBuf.vt = VT_BSTR;
	pvPszBuf.bstrVal = SysAllocString(ucr::toUTF16(text).c_str());
	// argument transformed text 
	VARIANT vTransformed;
	vTransformed.vt = VT_BSTR;
	vTransformed.bstrVal = NULL;

	// invoke method by ordinal
	// BSTR customFunction(BSTR text)
	HRESULT h = ::safeInvokeA(piScript, &vTransformed, fncId, opFxn[1], pvPszBuf);

	if (! FAILED(h))
	{
		text = ucr::toTString(vTransformed.bstrVal);
		changed = true;
	}
	else
		changed = false;

	// clear the returned variant
	VariantClear(&vTransformed);

	return (! FAILED(h));
}

bool InvokeIsFolder(const String& path, IDispatch *piScript)
{
	// argument text  
	VARIANT vbstrPath;
	vbstrPath.vt = VT_BSTR;
	vbstrPath.bstrVal = SysAllocString(ucr::toUTF16(path).c_str());

	VARIANT vboolHandled;
	vboolHandled.vt = VT_BOOL;
	vboolHandled.boolVal = false;

	// invoke method by name, reverse order for arguments
	// VARIANT_BOOL ShowSettingsDialog()
	HRESULT h = ::safeInvokeW(piScript, &vboolHandled, L"IsFolder", opFxn[1], vbstrPath);
	bool bSuccess = ! FAILED(h) && vboolHandled.boolVal;

	// clear the returned variant
	VariantClear(&vboolHandled);
	VariantClear(&vbstrPath);

	return (bSuccess);
}

bool InvokeShowSettingsDialog(IDispatch *piScript)
{
	VARIANT vboolHandled;
	vboolHandled.vt = VT_BOOL;
	vboolHandled.boolVal = false;

	// invoke method by name, reverse order for arguments
	// VARIANT_BOOL ShowSettingsDialog()
	HRESULT h = ::safeInvokeW(piScript, &vboolHandled, L"ShowSettingsDialog", opFxn[0]);
	bool bSuccess = ! FAILED(h) && vboolHandled.boolVal;

	// clear the returned variant
	VariantClear(&vboolHandled);

	return (bSuccess);
}