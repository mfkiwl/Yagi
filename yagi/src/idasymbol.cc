#include "idasymbol.hh"
#include "base.hh"
#include "exception.hh"
#include "idatool.hh"
#include "idatype.hh"
#include <idp.hpp>
#include <frame.hpp>
#include <struct.hpp>
#include <name.hpp>
#include <sstream>

namespace yagi 
{
	/**********************************************************************/
	std::optional<std::unique_ptr<SymbolInfo>> IdaSymbolInfoFactory::find(uint64_t ea)
	{
		qstring name;
		if (get_name(&name, ea) == 0 || name.size() == 0)
		{
			return std::nullopt;
		}
		return std::make_unique<IdaSymbolInfo>(ea, name.c_str());
	}

	/**********************************************************************/
	std::optional<std::unique_ptr<FunctionSymbolInfo>> IdaSymbolInfoFactory::find_function(uint64_t ea)
	{
		auto idaFunc = get_func(ea);
		if (idaFunc == nullptr)
		{
			return std::nullopt;
		}

		qstring idaName;
		get_short_name(&idaName, idaFunc->start_ea);
		auto beginParameter = idaName.find("(");
		auto functionName = split(idaName.substr(0, beginParameter).c_str(), ' ').back();

		return std::make_unique<IdaFunctionSymbolInfo>(std::make_unique<IdaSymbolInfo>(idaFunc->start_ea, functionName));
	}

	/**********************************************************************/
	IdaSymbolInfo::IdaSymbolInfo(uint64_t ea, std::string name)
		: SymbolInfo(ea, name) 
	{}

	/**********************************************************************/
	bool IdaSymbolInfo::isFunction() const noexcept
	{
		auto idaFunc = get_func(m_ea);
		return idaFunc != nullptr && idaFunc->start_ea == m_ea;
	}

	/**********************************************************************/
	bool IdaSymbolInfo::isImport() const noexcept
	{
		std::string importName = m_name;
		if (importName.length() > 6 && importName.substr(0, 6) == IMPORT_PREFIX)
		{
			importName = importName.substr(6, importName.length() - 6);
		}

		for (uint i = 0; i < get_import_module_qty(); i++)
		{
			auto found = enum_import_names(i,
				[](ea_t ea, const char* name, uval_t ord, void* param) {
					if (name != nullptr && *static_cast<std::string*>(param) == name)
					{
						return 0;
					}
					return 1;
				}, &importName
			);

			if (found != -1 && found != 1)
			{
				return true;
			}
		}
		return false;
	}

	/**********************************************************************/
	bool IdaSymbolInfo::isLabel() const noexcept
	{
		xrefblk_t xr;
		for (bool success = xr.first_to((ea_t)m_ea, XREF_ALL); success; success = xr.next_to()) {
			if (xr.iscode == 0) {
				break;
			}
			if (xr.type != fl_JN) {
				continue;
			}
			return true;
		}

		return false;
	}

	/**********************************************************************/
	bool IdaSymbolInfo::isReadOnly() const noexcept
	{
		auto seg = getseg(m_ea);
		qstring idaName;
		if (get_segm_name(&idaName, seg))
		{
			// assuming that .data segment are read only to improve static analysis
			if (idaName == ".data")
			{
				return true;
			}
		}

		return seg->perm == SEGPERM_READ || seg->perm == SEGPERM_READ + SEGPERM_EXEC;
	}

	/**********************************************************************/
	uint64_t IdaSymbolInfo::getFunctionSize() const
	{
		auto function = get_func(m_ea);
		if (function == nullptr || function->start_ea != m_ea)
		{
			throw SymbolIsNotAFunction(m_name);
		}

		return function->end_ea - function->start_ea;
	}

	/**********************************************************************/
	std::string IdaSymbolInfo::getName() const
	{
		qstring pname;
		if (m_name.substr(0, 4) == "sub_" || !cleanup_name(&pname, m_ea, m_name.c_str()))
		{
			pname = m_name.c_str();
		}

		qstring idaName = demangle_name(pname.c_str(), 0x0EA3BE67);
		if (idaName != "")
		{
			auto pp = idaName.find('(', 0);
			if (pp == qstring::npos)
			{
				pp = idaName.size();
			}

			pname = idaName.substr(0, pp);
		}

		// Mark import symbol with IDA convention
		if (isImport()) {
			return IMPORT_PREFIX + pname.c_str();
		}

		return pname.c_str();
	}

	/**********************************************************************/
	std::optional<std::string> IdaFunctionSymbolInfo::findStackVar(uint64_t offset, uint32_t addrSize)
	{
		auto idaFunc = get_func(m_symbol->getAddress());
		auto frame = get_frame(idaFunc);
		for (uint32_t i = 0; i < frame->memqty; i++)
		{
			auto member = frame->members[i];
			auto name = std::string(get_struc_name(member.id, STRNFL_REGEX).c_str());
			auto sofset = member.get_soff() - (idaFunc->frsize + idaFunc->frregs);
			if (sofset == offset || (addrSize == 4 && ((uint32_t)sofset == (uint32_t)offset)))
			{
				auto pp = name.find(".");
				if (pp != std::string::npos)
				{
					return name.substr(pp + 1);
				}
				return name;
			}
		}
		return std::nullopt;
	}

	/**********************************************************************/
	std::optional<std::string> IdaFunctionSymbolInfo::findRegVar(const std::string& name)
	{
		std::stringstream ss;
		ss << "$ " << to_hex(m_symbol->getAddress()) << ".yagireg." << name;
		netnode n(ss.str().c_str(), 0, true);
		
		qstring res;
		auto size = n.valstr(&res);

		if (res.size() == 0)
		{
			return std::nullopt;
		}

		return res.c_str();
	}

	/**********************************************************************/
	void IdaFunctionSymbolInfo::saveRegVar(const std::string& name, const std::string& value)
	{
		std::stringstream ss;
		ss << "$ " << to_hex(m_symbol->getAddress()) << ".yagireg." << name;
		netnode n(ss.str().c_str(), 0, true);
		n.set(value.c_str());
	}

	/**********************************************************************/
	void IdaFunctionSymbolInfo::saveSymbolType(const std::string& name, const TypeInfo& newType, const MemoryLocation& loc)
	{
		std::stringstream ss;
		ss << "$ " << to_hex(m_symbol->getAddress()) << ".yagitype." << name;
		netnode n(ss.str().c_str(), 0, true);
		n.set(newType.getName().c_str());
	}

	/**********************************************************************/
	std::optional<std::unique_ptr<TypeInfo>> IdaFunctionSymbolInfo::findSymbolType(const std::string& name)
	{
		std::stringstream ss;
		ss << "$ " << to_hex(m_symbol->getAddress()) << ".yagitype." << name;
		netnode n(ss.str().c_str(), 0, true);

		qstring res;
		auto size = n.valstr(&res);

		if (res.size() == 0)
		{
			return std::nullopt;
		}

		return IdaTypeInfoFactory().build_decl(res.c_str());
	}
} // end of namespace yagi