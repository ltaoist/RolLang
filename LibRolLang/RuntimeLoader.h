#pragma once
#include <vector>
#include <cassert>
#include <memory>
#include <algorithm>
#include "Assembly.h"
#include "Exceptions.h"
#include "Spinlock.h"

class RuntimeLoader;
struct RuntimeType;
struct RuntimeFunction;

struct AssemblyList
{
	std::vector<Assembly> Assemblies;
};

struct LoadingArguments
{
	std::string Assembly;
	std::size_t Id;
	std::vector<RuntimeType*> Arguments;

	bool operator == (const LoadingArguments &b) const
	{
		return Assembly == b.Assembly && Id == b.Id && Arguments == b.Arguments;
	}

	bool operator != (const LoadingArguments &b) const
	{
		return !(*this == b);
	}
};

struct RuntimeType
{
	struct RuntimeFieldInfo
	{
		RuntimeType* Type;
		std::size_t Offset;
		std::size_t Length;
	};

	RuntimeLoader* Parent;
	LoadingArguments Args;
	std::size_t TypeId;

	TypeStorageMode Storage;
	std::vector<RuntimeFieldInfo> Fields;
	std::size_t Alignment;
	std::size_t Size;

	RuntimeFunction* GCFinalizer;
	void* StaticPointer;

	std::size_t GetStorageSize()
	{
		return Storage == TypeStorageMode::TSM_REF ? sizeof(void*) : Size;
	}

	std::size_t GetStorageAlignment()
	{
		return Storage == TypeStorageMode::TSM_REF ? sizeof(void*) : Alignment;
	}
};

struct RuntimeFunctionCode
{
	std::string AssemblyName;
	std::size_t Id;
	std::vector<unsigned char> Instruction;
	std::vector<unsigned char> ConstantData;
	std::vector<FunctionConst> ConstantTable;
	std::vector<FunctionLocal> LocalVariables;
};

struct RuntimeFunction
{
	RuntimeLoader* Parent;
	LoadingArguments Args;
	std::size_t FunctionId;

	std::shared_ptr<RuntimeFunctionCode> Code;

	std::vector<RuntimeType*> ReferencedType;
	std::vector<RuntimeFunction*> ReferencedFunction;

	RuntimeType* ReturnValue;
	std::vector<RuntimeType*> Parameters;
};

struct RuntimeFunctionCodeStorage
{
	std::vector<std::shared_ptr<RuntimeFunctionCode>> Data;
};

class RuntimeLoader
{
	/*
	 * About the loading process:
	 *
	 * Each reference type will undergo the following stages:
	 * 1. LoadTypeInternal. Then move to _loadingRefTypes. Pointer available.
	 * 2. LoadFields. Then move to _postLoadingTypes.
	 * 3. PostLoadType. Then move to _finishedLoadingTypes.
	 * 4. (After all finished.) MoveFinishedObjects. Then move to _loadedTypes.
	 *
	 * Each value type will undergo the following stages:
	 * 1. LoadTypeInternal.
	 * 1.1. Put into _loadingTypes stack to avoid cyclic dependence.
	 * 1.2. LoadFields. Then move to _postLoadingTypes.
	 * 1.3. Remove from _loadingTypes stack.
	 * 1.4. Pointer available.
	 * 2. PostLoadType. Then move to _finishedLoadingTypes.
	 * 3. (After all finished.) MoveFinishedObjects. Then move to _loadedTypes.
	 *
	 * Each function will undergo the following stages:
	 * 1. LoadFunctionInternal. Then move to _loadingFunctions. Pointer available.
	 * 2. PostLoadFunction. Then move to _finishedLoadingFunctions.
	 * 3. (After all finished.) MoveFinishedObjects. Then move to _loadedFunctions.
	 *
	 * OnXXXLoaded virtual functions are called within MoveFinishedObjects to allow
	 * subclasses to do custom registration work. After all objects are processed,
	 * all objects are moved to loaded list. If any function call fails by throwing
	 * an InternalException, no object will be moved to loaded list and the API fails.
	 *
	 */

public:
	RuntimeLoader(AssemblyList assemblies)
		: _assemblies(std::move(assemblies))
	{
	}

public:
	RuntimeType* GetType(const LoadingArguments& args, std::string& err)
	{
		std::lock_guard<Spinlock> lock(_loaderLock);
		for (auto& t : _loadedTypes)
		{
			if (t && t->Args == args)
			{
				return t.get();
			}
		}
		return LoadTypeNoLock(args, err);
	}

	RuntimeFunction* GetFunction(const LoadingArguments& args, std::string& err)
	{
		std::lock_guard<Spinlock> lock(_loaderLock);
		for (auto& f : _loadedFunctions)
		{
			if (f->Args == args)
			{
				return f.get();
			}
		}
		return LoadFunctionNoLock(args, err);
	}

	RuntimeType* AddNativeType(const std::string& assemblyName, const std::string& name,
		std::size_t size, std::size_t alignment, std::string& err)
	{
		std::lock_guard<Spinlock> lock(_loaderLock);
		try
		{
			auto id = FindNativeId(FindAssembly(assemblyName)->NativeTypes, name);
			if (id == SIZE_MAX)
			{
				err = "Native type not found";
				return nullptr;
			}
			return AddNativeTypeInternal(assemblyName, id, size, alignment);
		}
		catch (std::exception& ex)
		{
			err = ex.what();
		}
		catch (...)
		{
			err = "Unknown exception in adding native type";
		}
		return nullptr;
	}

public:
	RuntimeType* GetTypeById(std::uint32_t id)
	{
		std::lock_guard<Spinlock> lock(_loaderLock);
		if (id >= _loadedTypes.size())
		{
			return nullptr;
		}
		return _loadedTypes[id].get();
	}

	RuntimeFunction* GetFunctionById(std::uint32_t id)
	{
		std::lock_guard<Spinlock> lock(_loaderLock);
		if (id > _loadedFunctions.size())
		{
			return nullptr;
		}
		return _loadedFunctions[id].get();
	}

	std::size_t FindExportType(const std::string& assemblyName, const std::string& n)
	{
		auto a = FindAssembly(assemblyName);
		for (auto& e : a->ExportTypes)
		{
			if (e.ExportName == n) return e.InternalId;
		}
		return SIZE_MAX;
	}

	std::size_t FindExportFunction(const std::string& assemblyName, const std::string& n)
	{
		auto a = FindAssembly(assemblyName);
		for (auto& e : a->ExportFunctions)
		{
			if (e.ExportName == n) return e.InternalId;
		}
		return SIZE_MAX;
	}

private:
	RuntimeType* LoadTypeNoLock(const LoadingArguments& args, std::string& err)
	{
		ClearLoadingLists();
		RuntimeType* ret = nullptr;
		try
		{
			auto ret2 = LoadTypeInternal(args);
			ProcessLoadingLists();
			MoveFinishedObjects();
			ret = ret2;
		}
		catch (std::exception& ex)
		{
			err = ex.what();
		}
		catch (...)
		{
			err = "Unknown exception in loading type";
		}
		ClearLoadingLists();
		return ret;
	}

	RuntimeFunction* LoadFunctionNoLock(const LoadingArguments& args, std::string& err)
	{
		ClearLoadingLists();
		RuntimeFunction* ret = nullptr;
		try
		{
			auto ret2 = LoadFunctionInternal(args);
			ProcessLoadingLists();
			MoveFinishedObjects();
			ret = ret2;
		}
		catch (std::exception& ex)
		{
			err = ex.what();
		}
		catch (...)
		{
			err = "Unknown exception in loading function";
		}
		ClearLoadingLists();
		return ret;
	}

private:
	RuntimeType* AddNativeTypeInternal(const std::string& assemblyName, std::size_t id,
		std::size_t size, std::size_t alignment)
	{
		auto a = FindAssembly(assemblyName);
		auto& type = a->Types[id];
		if (type.Generic.Parameters.size())
		{
			throw RuntimeLoaderException("Native type cannot be generic");
		}
		if (type.GCMode != TSM_VALUE)
		{
			throw RuntimeLoaderException("Internal type can only be value type");
		}
		if (type.OnFinalize >= type.Generic.Functions.size())
		{
			throw RuntimeLoaderException("Invalid function reference");
		}
		if (type.Generic.Functions[type.OnFinalize].Type != REF_EMPTY)
		{
			throw RuntimeLoaderException("Internal type cannot have Finalizer");
		}
		auto rt = std::make_unique<RuntimeType>();
		rt->Parent = this;
		rt->TypeId = _nextTypeId++;
		rt->Args.Assembly = assemblyName;
		rt->Args.Id = id;
		rt->Storage = TSM_VALUE;
		rt->Size = size;
		rt->Alignment = alignment;
		rt->GCFinalizer = nullptr;
		rt->StaticPointer = nullptr;

		auto ret = rt.get();
		AddLoadedType(std::move(rt));
		return ret;
	}

	void ClearLoadingLists()
	{
		_loadingTypes.clear();
		_loadingFunctions.clear();
		_loadingRefTypes.clear();
		_postLoadingTypes.clear();
		_finishedLoadingTypes.clear();
		_finishedLoadingFunctions.clear();
	}

	void MoveFinishedObjects()
	{
		for (auto& t : _finishedLoadingTypes)
		{
			OnTypeLoaded(t.get());
		}
		for (auto& f : _finishedLoadingFunctions)
		{
			OnFunctionLoaded(f.get());
		}
		while (_finishedLoadingTypes.size() > 0)
		{
			auto t = std::move(_finishedLoadingTypes.back());
			AddLoadedType(std::move(t));
			_finishedLoadingTypes.pop_back();
		}
		while (_finishedLoadingFunctions.size() > 0)
		{
			auto f = std::move(_finishedLoadingFunctions.back());
			AddLoadedFunction(std::move(f));
			_finishedLoadingFunctions.pop_back();
		}
	}

	void ProcessLoadingLists()
	{
		assert(_loadingTypes.size() == 0);

		while (true)
		{
			if (_loadingRefTypes.size())
			{
				auto t = std::move(_loadingRefTypes.back());
				_loadingRefTypes.pop_back();
				LoadFields(std::move(t), nullptr);
				assert(_loadingTypes.size() == 0);
				continue;
			}
			if (_postLoadingTypes.size())
			{
				auto t = std::move(_postLoadingTypes.back());
				_postLoadingTypes.pop_back();
				PostLoadType(std::move(t));
				assert(_loadingTypes.size() == 0);
				continue;
			}
			if (_loadingFunctions.size())
			{
				auto t = std::move(_loadingFunctions.back());
				_loadingFunctions.pop_back();
				PostLoadFunction(std::move(t));
				assert(_loadingTypes.size() == 0);
				continue;
			}
			break;
		}
	}

	void CheckGenericArguments(GenericDeclaration& g, const LoadingArguments& args)
	{
		if (g.Parameters.size() != args.Arguments.size())
		{
			throw RuntimeLoaderException("Invalid generic arguments");
		}
		if (std::any_of(args.Arguments.begin(), args.Arguments.end(),
			[](RuntimeType* t) { return t == nullptr; }))
		{
			throw RuntimeLoaderException("Invalid generic arguments");
		}
		//TODO argument constrain check
	}

	RuntimeType* LoadTypeInternal(const LoadingArguments& args)
	{
		for (auto& t : _loadedTypes)
		{
			if (t && t->Args == args)
			{
				return t.get();
			}
		}
		for (auto& t : _finishedLoadingTypes)
		{
			if (t->Args == args)
			{
				return t.get();
			}
		}
		for (auto& t : _postLoadingTypes)
		{
			if (t->Args == args)
			{
				return t.get();
			}
		}
		for (auto& t : _loadingRefTypes)
		{
			if (t->Args == args)
			{
				return t.get();
			}
		}
		for (auto& t : _loadingTypes)
		{
			if (t->Args == args)
			{
				throw RuntimeLoaderException("Cyclic type dependence");
			}
		}

		auto typeTemplate = FindTypeTemplate(args.Assembly, args.Id);
		CheckGenericArguments(typeTemplate->Generic, args);

		if (typeTemplate->GCMode == TSM_REF)
		{
			auto t = std::make_unique<RuntimeType>();
			t->Parent = this;
			t->Args = args;
			t->TypeId = _nextTypeId++;
			t->Storage = typeTemplate->GCMode;
			t->StaticPointer = nullptr;
			RuntimeType* ret = t.get();
			_loadingRefTypes.push_back(std::move(t));
			return ret;
		}
		else
		{
			auto t = std::make_unique<RuntimeType>();
			_loadingTypes.push_back(t.get());
			t->Parent = this;
			t->Args = args;
			t->TypeId = _nextTypeId++;
			t->Storage = typeTemplate->GCMode;
			t->StaticPointer = nullptr;

			auto ret = LoadFields(std::move(t), typeTemplate);
			assert(_loadingTypes.back() == ret);
			_loadingTypes.pop_back();
			return ret;
		}
	}

	RuntimeFunction* LoadFunctionInternal(const LoadingArguments& args)
	{
		for (auto& ff : _loadedFunctions)
		{
			if (ff && ff->Args == args)
			{
				return ff.get();
			}
		}
		for (auto& ff : _finishedLoadingFunctions)
		{
			if (ff->Args == args)
			{
				return ff.get();
			}
		}
		for (auto& ff : _loadingFunctions)
		{
			if (ff->Args == args)
			{
				return ff.get();
			}
		}

		auto f = std::make_unique<RuntimeFunction>();
		auto ret = f.get();
		_loadingFunctions.push_back(std::move(f));
		ret->Args = args;
		ret->Parent = this;
		ret->FunctionId = _nextFunctionId++;
		ret->Code = GetCode(args.Assembly, args.Id);

		auto funcTemplate = FindFunctionTemplate(args.Assembly, args.Id);
		CheckGenericArguments(funcTemplate->Generic, args);
		return ret;
	}

	RuntimeType* LoadFields(std::unique_ptr<RuntimeType> type, Type* typeTemplate)
	{
		Type* tt = typeTemplate;
		if (tt == nullptr)
		{
			tt = FindTypeTemplate(type->Args.Assembly, type->Args.Id);
		}
		std::vector<RuntimeType*> fields;
		for (auto typeId : tt->Fields)
		{
			auto fieldType = LoadRefType(type->Args, tt->Generic, typeId);
			if (!fieldType)
			{
				//Only goes here if REF_EMPTY is specified.
				throw RuntimeLoaderException("Invalid field type");
			}
			fields.push_back(fieldType);
		}

		std::size_t offset = 0, totalAlignment = 1;
		for (std::size_t i = 0; i < fields.size(); ++i)
		{
			auto ftype = fields[i];
			std::size_t len, alignment;
			if (ftype->Storage == TSM_REF)
			{
				len = alignment = sizeof(void*);
			}
			else if (ftype->Storage == TSM_VALUE)
			{
				len = ftype->Size;
				alignment = ftype->Alignment;
			}
			else
			{
				throw RuntimeLoaderException("Invalid field type");
			}
			offset = (offset + alignment - 1) / alignment * alignment;
			totalAlignment = alignment > totalAlignment ? alignment : totalAlignment;
			type->Fields.push_back({ ftype, offset, len });
			offset += len;
		}
		type->Size = offset == 0 ? 1 : offset;
		type->Alignment = totalAlignment;
		auto ret = type.get();
		_postLoadingTypes.emplace_back(std::move(type));
		return ret;
	}

	void PostLoadType(std::unique_ptr<RuntimeType> type)
	{
		auto typeTemplate = FindTypeTemplate(type->Args.Assembly, type->Args.Id);
		type->GCFinalizer = LoadRefFunction(type->Args, typeTemplate->Generic, typeTemplate->OnFinalize);
		if (type->Storage == TSM_GLOBAL)
		{
			std::size_t alignment = type->GetStorageAlignment();
			std::size_t totalSize = type->GetStorageSize() + alignment;
			std::unique_ptr<char[]> ptr = std::make_unique<char[]>(totalSize);
			uintptr_t rawPtr = (uintptr_t)ptr.get();
			uintptr_t alignedPtr = (rawPtr + alignment - 1) / alignment * alignment;
			type->StaticPointer = (void*)alignedPtr;
		}
		_finishedLoadingTypes.emplace_back(std::move(type));
	}

	void PostLoadFunction(std::unique_ptr<RuntimeFunction> func)
	{
		//TODO Optimize loading. Directly find the cloned func/type.
		auto funcTemplate = FindFunctionTemplate(func->Args.Assembly, func->Args.Id);
		for (std::size_t i = 0; i < funcTemplate->Generic.Types.size(); ++i)
		{
			func->ReferencedType.push_back(LoadRefType(func->Args, funcTemplate->Generic, i));
		}
		for (std::size_t i = 0; i < funcTemplate->Generic.Functions.size(); ++i)
		{
			func->ReferencedFunction.push_back(LoadRefFunction(func->Args, funcTemplate->Generic, i));
		}
		func->ReturnValue = func->ReferencedType[funcTemplate->ReturnValue.TypeId];
		for (std::size_t i = 0; i < funcTemplate->Parameters.size(); ++i)
		{
			func->Parameters.push_back(func->ReferencedType[funcTemplate->Parameters[i].TypeId]);
		}
		auto ptr = func.get();
		_finishedLoadingFunctions.emplace_back(std::move(func));
	}

protected:
	virtual void OnTypeLoaded(RuntimeType* type)
	{
	}

	virtual void OnFunctionLoaded(RuntimeFunction* func)
	{
	}

private:
	RuntimeType* LoadRefType(const LoadingArguments& args, GenericDeclaration& g, std::size_t typeId)
	{
		if (typeId >= g.Types.size())
		{
			throw RuntimeLoaderException("Invalid type reference");
		}
		auto type = g.Types[typeId];
	loadClone:
		switch (type.Type)
		{
		case REF_EMPTY:
			return nullptr;
		case REF_CLONE:
			if (type.Index >= g.Types.size())
			{
				throw RuntimeLoaderException("Invalid type reference");
			}
			typeId = type.Index;
			type = g.Types[type.Index];
			goto loadClone;
		case REF_ASSEMBLY:
			return LoadDependentType(args.Assembly, type.Index, args, g, typeId);
		case REF_IMPORT:
			return LoadDependentTypeImport(args.Assembly, type.Index, args, g, typeId);
		case REF_ARGUMENT:
			if (type.Index >= args.Arguments.size())
			{
				throw RuntimeLoaderException("Invalid type reference");
			}
			return args.Arguments[type.Index];
		case REF_CLONETYPE:
		default:
			throw RuntimeLoaderException("Invalid type reference");
		}
	}

	RuntimeType* LoadDependentType(const std::string& assembly, std::size_t id,
		const LoadingArguments& lastArgs, GenericDeclaration& g, std::size_t refListIndex,
		std::size_t checkArgSize = SIZE_MAX)
	{
		LoadingArguments newArgs;
		newArgs.Assembly = assembly;
		newArgs.Id = id;
		for (std::size_t i = refListIndex + 1; i < g.Types.size(); ++i)
		{
			if (g.Types[i].Type == REF_EMPTY) break; //Use REF_Empty as the end of arg list
			newArgs.Arguments.push_back(LoadRefType(lastArgs, g, i));
		}
		if (checkArgSize != SIZE_MAX)
		{
			if (newArgs.Arguments.size() != checkArgSize)
			{
				throw RuntimeLoaderException("Invalid generic argument list");
			}
		}
		return LoadTypeInternal(newArgs);
	}

	RuntimeType* LoadDependentTypeImport(const std::string& assembly, std::size_t id,
		const LoadingArguments& lastArgs, GenericDeclaration& g, std::size_t refListIndex)
	{
		auto a = FindAssembly(assembly);
		if (id >= a->ImportTypes.size())
		{
			throw RuntimeLoaderException("Invalid type reference");
		}
		auto i = a->ImportTypes[id];
		auto a2 = FindAssembly(i.AssemblyName);
		for (auto e : a2->ExportTypes)
		{
			if (e.ExportName == i.ImportName)
			{
				return LoadDependentType(i.AssemblyName, e.InternalId,
					lastArgs, g, refListIndex, i.GenericParameters);
			}
		}
		throw RuntimeLoaderException("Import type not found");
	}

	RuntimeFunction* LoadRefFunction(const LoadingArguments& args,
		GenericDeclaration& g, std::size_t funcId)
	{
		if (funcId >= g.Functions.size())
		{
			throw RuntimeLoaderException("Invalid function reference");
		}
		auto func = g.Functions[funcId];
	loadClone:
		switch (func.Type)
		{
		case REF_EMPTY:
			return nullptr;
		case REF_CLONE:
			if (func.Index >= g.Functions.size())
			{
				throw RuntimeLoaderException("Invalid function reference");
			}
			funcId = func.Index;
			func = g.Functions[func.Index];
			goto loadClone;
		case REF_ASSEMBLY:
			return LoadDependentFunction(args.Assembly, func.Index, args, g, funcId);
		case REF_IMPORT:
			return LoadDependentFunctionImport(args.Assembly, func.Index, args, g, funcId);
		case REF_ARGUMENT:
		case REF_CLONETYPE:
		default:
			throw RuntimeLoaderException("Invalid function reference");
		}
	}

	RuntimeFunction* LoadDependentFunction(const std::string& assembly, std::size_t id,
		const LoadingArguments& lastArgs, GenericDeclaration& g, std::size_t refListIndex,
		std::size_t checkArgSize = SIZE_MAX)
	{
		LoadingArguments newArgs;
		newArgs.Assembly = assembly;
		newArgs.Id = id;
		for (std::size_t i = refListIndex + 1; i < g.Functions.size(); ++i)
		{
			if (g.Functions[i].Type == REF_EMPTY) break;
			if (g.Functions[i].Type != REF_CLONETYPE)
			{
				throw RuntimeLoaderException("Invalid generic function argument");
			}
			newArgs.Arguments.push_back(LoadRefType(lastArgs, g, g.Functions[i].Index));
		}
		if (checkArgSize != SIZE_MAX)
		{
			if (newArgs.Arguments.size() != checkArgSize)
			{
				throw RuntimeLoaderException("Invalid generic argument list");
			}
		}
		return LoadFunctionInternal(newArgs);
	}

	RuntimeFunction* LoadDependentFunctionImport(const std::string& assembly, std::size_t id,
		const LoadingArguments& lastArgs, GenericDeclaration& g, std::size_t refListIndex)
	{
		auto a = FindAssembly(assembly);
		if (id >= a->ImportFunctions.size())
		{
			throw RuntimeLoaderException("Invalid function reference");
		}
		auto i = a->ImportFunctions[id];
		auto a2 = FindAssembly(i.AssemblyName);
		for (auto e : a2->ExportFunctions)
		{
			if (e.ExportName == i.ImportName)
			{
				return LoadDependentFunction(i.AssemblyName, e.InternalId,
					lastArgs, g, refListIndex, i.GenericParameters);
			}
		}
		throw RuntimeLoaderException("Import function not found");
	}

protected:
	Assembly* FindAssembly(const std::string& name)
	{
		for (auto& a : _assemblies.Assemblies)
		{
			if (a.AssemblyName == name)
			{
				return &a;
			}
		}
		throw RuntimeLoaderException("Referenced assembly not found");
	}

	Type* FindTypeTemplate(const std::string& assembly, std::size_t id)
	{
		auto a = FindAssembly(assembly);
		if (id >= a->Types.size())
		{
			throw RuntimeLoaderException("Invalid type reference");
		}
		return &a->Types[id];
	}

	Function* FindFunctionTemplate(const std::string& assembly, std::size_t id)
	{
		auto a = FindAssembly(assembly);
		if (id >= a->Functions.size())
		{
			throw RuntimeLoaderException("Invalid function reference");
		}
		return &a->Functions[id];
	}

	std::size_t FindNativeId(const std::vector<AssemblyExport>& list,
		const std::string name)
	{
		for (std::size_t i = 0; i < list.size(); ++i)
		{
			if (list[i].ExportName == name)
			{
				return list[i].InternalId;
			}
		}
		return SIZE_MAX;
	}

private:
	std::shared_ptr<RuntimeFunctionCode> GetCode(const std::string& a, std::size_t id)
	{
		for (auto& c : _codeStorage.Data)
		{
			if (c->AssemblyName == a && c->Id == id)
			{
				return c;
			}
		}
		auto f = FindFunctionTemplate(a, id);
		if (f->Instruction.size() == 0 && f->ConstantData.size() == 0 &&
			f->ConstantTable.size() == 0)
		{
			return nullptr;
		}
		auto ret = std::make_shared<RuntimeFunctionCode>();
		ret->AssemblyName = a;
		ret->Id = id;
		ret->Instruction = f->Instruction;
		ret->ConstantData = f->ConstantData;
		ret->ConstantTable = f->ConstantTable;
		ret->LocalVariables = f->Locals;
		_codeStorage.Data.push_back(ret);
		return std::move(ret);
	}

	void AddLoadedType(std::unique_ptr<RuntimeType> t)
	{
		auto id = t->TypeId;
		if (id < _loadedTypes.size())
		{
			assert(_loadedTypes[id] == nullptr);
			_loadedTypes[id] = std::move(t);
		}
		else
		{
			while (id > _loadedTypes.size())
			{
				_loadedTypes.push_back(nullptr);
			}
			_loadedTypes.emplace_back(std::move(t));
		}
	}

	void AddLoadedFunction(std::unique_ptr<RuntimeFunction> f)
	{
		auto id = f->FunctionId;
		if (id < _loadedFunctions.size())
		{
			assert(_loadedFunctions[id] == nullptr);
			_loadedFunctions[id] = std::move(f);
		}
		else
		{
			while (id > _loadedFunctions.size())
			{
				_loadedFunctions.push_back(nullptr);
			}
			_loadedFunctions.emplace_back(std::move(f));
		}
	}

protected:
	//We don't expect loader to run very often. A simple spinlock
	//should be enough.
	Spinlock _loaderLock;

private:
	AssemblyList _assemblies;

	std::vector<std::unique_ptr<RuntimeType>> _loadedTypes;
	std::vector<std::unique_ptr<RuntimeFunction>> _loadedFunctions;
	RuntimeFunctionCodeStorage _codeStorage;

	std::vector<RuntimeType*> _loadingTypes;
	std::vector<std::unique_ptr<RuntimeType>> _loadingRefTypes;
	std::vector<std::unique_ptr<RuntimeType>> _postLoadingTypes;
	std::vector<std::unique_ptr<RuntimeFunction>> _loadingFunctions;
	std::vector<std::unique_ptr<RuntimeType>> _finishedLoadingTypes;
	std::vector<std::unique_ptr<RuntimeFunction>> _finishedLoadingFunctions;

	std::uint32_t _nextFunctionId = 1, _nextTypeId = 1;
};
