/*
This file is part of Darling.

Copyright (C) 2015 Lubos Dolezel

Darling is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Darling is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Darling.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "MachOMgr.h"
#include <cassert>
#include <algorithm>
#include <bits/wordsize.h>
#include "MachOObject.h"
#include "NativeObject.h"
#include <util/debug.h>
#include <dlfcn.h>

namespace Darling {

bool MachOMgr::m_bTerminated = false;

extern "C" char** __darwin_environ;

MachOMgr::MachOMgr()
: m_mainModule(nullptr), m_bindAtLaunch(false), m_printInitializers(false),
  m_printLibraries(false), m_printSegments(false), m_printBindings(false),
  m_printRpathExpansion(false), m_loadAny(false), m_forceFlatNamespace(false),
  m_addedDefaultLoader(false), m_destroying(false)
{
	__darwin_environ = environ;
}

MachOMgr::~MachOMgr()
{
	m_destroying = true;
	
	if (m_mainModule)
		m_mainModule->delRef();

	while (!m_loadablesInOrder.empty())
	{
		LoadableObject* obj = m_loadablesInOrder.front();
		
		if (dynamic_cast<NativeObject*>(obj) != nullptr)
			obj->unload();
		else
			m_loadablesInOrder.pop_front(); // let ld.so do it on its own, otherwise crashes may follow
	}
	
	m_bTerminated = true;
}

void MachOMgr::atexit()
{
	for (LoadableObject* obj : m_loadablesInOrder)
	{
		obj->atExit();
	}
}

MachOMgr* MachOMgr::instance()
{
	static MachOMgr obj;
	return &obj;
}

void* MachOMgr::maxAddress() const
{
	Darling::RWMutexReadLock l(m_lock);
	
	if (m_objects.empty())
	{
		return nullptr;
	}
	else
	{
		auto it = m_objects.end();
		void* addr;
		
		it--;
		addr = it->second->maxAddress();
		
		assert(uintptr_t(addr) % getpagesize() == 0);
		
		return addr;
	}
}

MachOObject* MachOMgr::objectForAddress(void* addr)
{
	Darling::RWMutexReadLock l(m_lock);
	
	auto it = m_objects.upper_bound(addr);
	
	if (it == m_objects.begin())
		return nullptr;
	
	it--;

	if (it->second->maxAddress() < addr)
		return nullptr;

	return it->second;
}

void MachOMgr::registerLoadHook(LoaderHookFunc* func)
{
	Darling::RWMutexWriteLock l(m_lock);
	
	m_loadHooks.insert(func);
}

void MachOMgr::registerUnloadHook(LoaderHookFunc* func)
{
	Darling::RWMutexWriteLock l(m_lock);
	
	m_unloadHooks.insert(func);
}

void MachOMgr::deregisterLoadHook(LoaderHookFunc* func)
{
	Darling::RWMutexWriteLock l(m_lock);
	
	m_loadHooks.erase(func);
}

void MachOMgr::deregisterUnloadHook(LoaderHookFunc* func)
{
	Darling::RWMutexWriteLock l(m_lock);
	
	m_unloadHooks.erase(func);
}

void MachOMgr::addDefaultLoader()
{
//	if (!m_addedDefaultLoader)
//	{
//		add(new NativeObject(RTLD_DEFAULT, "<default>"));
//		m_addedDefaultLoader = true;
//	}
}

void MachOMgr::add(MachOObject* obj, bool mainModule)
{
	Darling::RWMutexWriteLock l(m_lock);

	if (mainModule)
		addDefaultLoader();
	
	m_objects[obj->baseAddress()] = obj;
	m_objectNames[obj->path()] = obj;
	m_objectHeaders[obj->getMachHeader()] = obj;
	m_objectsInOrder.push_back(obj);
	m_loadablesInOrder.push_back(obj);
	
	if (mainModule)
	{
		assert(m_mainModule == nullptr);
		m_mainModule = obj;
	}
}

void MachOMgr::notifyAdd(MachOObject* obj)
{
	assert(m_objects.find(obj->baseAddress()) != m_objects.end());

	for (LoaderHookFunc* func : m_loadHooks)
		func(obj->getMachHeader(), obj->slide());
}

void MachOMgr::remove(MachOObject* obj)
{
	Darling::RWMutexWriteLock l(m_lock);
	
	for (LoaderHookFunc* func : m_unloadHooks)
		func(obj->getMachHeader(), obj->slide());
	
	m_objects.erase(obj->baseAddress());
	m_objectNames.erase(obj->path());
	m_objectHeaders.erase(obj->getMachHeader());
	
	auto it = std::find(m_objectsInOrder.begin(), m_objectsInOrder.end(), obj);
	if (it != m_objectsInOrder.end())
		m_objectsInOrder.erase(it);

	auto itl = std::find(m_loadablesInOrder.begin(), m_loadablesInOrder.end(), obj);
	if (itl != m_loadablesInOrder.end())
		m_loadablesInOrder.erase(itl);
	
	if (m_mainModule == obj)
		m_mainModule = nullptr;
}

void MachOMgr::add(NativeObject* obj)
{
	LOG << "MachOMgr::add: "  << obj << " - " << obj->name() << std::endl;
	m_objectNames[obj->path()] = obj;
	m_loadablesInOrder.push_back(obj);
	// m_objectHeaders[(struct mach_header*) obj->baseAddress()] = obj;
	m_nativeRefToObject[obj->nativeRef()] = obj;
}

void MachOMgr::remove(NativeObject* obj)
{
	LOG << "MachOMgr::remove: "  << obj << " - " << obj->name() << std::endl;
	auto it = std::find(m_loadablesInOrder.begin(), m_loadablesInOrder.end(), obj);
	if (it != m_loadablesInOrder.end())
		m_loadablesInOrder.erase(it);
	//auto it2 = m_objectHeaders.find(obj->baseAddress());
	//if (it2 != m_objectHeaders.end())
	//	m_objectHeaders.erase(it2);
	m_objectNames.erase(obj->path());
	m_nativeRefToObject.erase(obj->nativeRef());
}

MachOObject* MachOMgr::objectByIndex(size_t index)
{
	Darling::RWMutexReadLock l(m_lock);
	
	if (index >= m_objectsInOrder.size())
		return nullptr;
	else
		return m_objectsInOrder[index];
}

MachOObject* MachOMgr::objectByHeader(struct mach_header* hdr)
{
	Darling::RWMutexReadLock l(m_lock);
	
	auto it = m_objectHeaders.find(hdr);
	if (it == m_objectHeaders.end())
		return nullptr;
	else
		return it->second;
}

NativeObject* MachOMgr::objectByNativeRef(void* nativeRef)
{
	auto it = m_nativeRefToObject.find(nativeRef);
	if (it == m_nativeRefToObject.end())
		return nullptr;
	else
		return it->second;
}

void* MachOMgr::getExportedSymbol(const std::string& symbolName, LoadableObject* nextAfter)
{
	Darling::RWMutexReadLock l(m_lock);
	void* weak = nullptr;
	bool isAfter = false;

	// TODO: add default loader if missing

	for (auto it = m_loadablesInOrder.begin(); it != m_loadablesInOrder.end(); it++)
	{
		void* p;

		if (nextAfter && !isAfter)
		{
			if (*it == nextAfter)
				isAfter = true;

			continue;
		}

		if (!(*it)->globalExports())
			continue;

		p = (*it)->getExportedSymbol(symbolName, true); // try non-weak only
		if (p)
			return p;
		
		if (!weak)
			weak = (*it)->getExportedSymbol(symbolName, false); // save the first weak export as a fallback
	}
	
	return weak;
}

LoadableObject* MachOMgr::lookup(const std::string& absolutePath)
{
	Darling::RWMutexReadLock l(m_lock);

	char path[PATH_MAX];
	
	realpath(absolutePath.c_str(), path);

	auto it = m_objectNames.find(path);
	if (it != m_objectNames.end())
		return it->second;
	else
		return nullptr;
}

bool MachOMgr::detectSysRootFromPath(std::string path)
{
	if (path.empty())
		return false;
	if (path[0] != '/')
	{
		char* rp = realpath(path.c_str(), nullptr);
		if (!rp)
			return false;

		path = rp;
		free(rp);
	}

	size_t pos = path.find("/usr/");
	if (pos != std::string::npos && pos != 0)
	{
		m_sysroot = path.substr(0, pos);
		return true;
	}

	return false;
}

#ifdef HAS_DEBUG_HELPERS
void MachOMgr::setUseTrampolines(bool useTrampolines, const std::string& funcInfo)
{
	delete m_pTrampolineMgr;
	
	if (useTrampolines)
	{
		m_pTrampolineMgr = new TrampolineMgr;
		m_pTrampolineMgr->loadFunctionInfo(funcInfo.c_str());
	}
	else
	{
		m_pTrampolineMgr = nullptr;
	}
}

void MachOMgr::setIgnoreMissingSymbols(bool ignoreMissingSymbols)
{
	delete m_pUndefMgr;

	if (ignoreMissingSymbols)
		m_pUndefMgr = new UndefMgr;
	else
		m_pUndefMgr = nullptr;
}
#else
void MachOMgr::setUseTrampolines(bool useTrampolines, const std::string& funcInfo)
{
}

void MachOMgr::setIgnoreMissingSymbols(bool ignoreMissingSymbols)
{
}
#endif

} // namespace Darling
