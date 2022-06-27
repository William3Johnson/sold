#include "mold.h"
#include "../archive-file.h"

namespace mold::macho {

template <typename E>
std::ostream &operator<<(std::ostream &out, const InputFile<E> &file) {
  if (file.archive_name.empty())
    out << path_clean(file.filename);
  else
    out << path_clean(file.archive_name) << "(" << path_clean(file.filename) + ")";
  return out;
}

template <typename E>
void InputFile<E>::clear_symbols() {
  for (Symbol<E> *sym : syms) {
    std::scoped_lock lock(sym->mu);
    if (sym->file == this) {
      sym->file = nullptr;
      sym->scope = SCOPE_LOCAL;
      sym->is_imported = false;
      sym->is_weak = false;
      sym->subsec = nullptr;
      sym->value = 0;
      sym->is_common = false;
    }
  }
}

template <typename E>
ObjectFile<E> *
ObjectFile<E>::create(Context<E> &ctx, MappedFile<Context<E>> *mf,
                      std::string archive_name) {
  ObjectFile<E> *obj = new ObjectFile<E>(mf);
  obj->archive_name = archive_name;
  obj->is_alive = archive_name.empty() || ctx.all_load;
  obj->is_hidden = ctx.hidden_l;
  ctx.obj_pool.emplace_back(obj);
  return obj;
};

template <typename E>
void ObjectFile<E>::parse(Context<E> &ctx) {
  if (get_file_type(this->mf) == FileType::LLVM_BITCODE) {
    // Open an compiler IR file
    load_lto_plugin(ctx);
    this->lto_module =
      ctx.lto.module_create_from_memory(this->mf->data, this->mf->size);
    if (!this->lto_module)
      Fatal(ctx) << *this << ": lto_module_create_from_memory failed";

    // Read a symbol table
    parse_lto_symbols(ctx);
    return;
  }

  parse_sections(ctx);
  parse_symbols(ctx);

  MachHeader &mach_hdr = *(MachHeader *)this->mf->data;
  if (mach_hdr.flags & MH_SUBSECTIONS_VIA_SYMBOLS)
    split_subsections_via_symbols(ctx);
  else
    init_subsections(ctx);

  sort(subsections, [](Subsection<E> *a, Subsection<E> *b) {
    return a->input_addr < b->input_addr;
  });

  fix_subsec_members(ctx);

  for (std::unique_ptr<InputSection<E>> &isec : sections)
    if (isec)
      isec->parse_relocations(ctx);

  if (unwind_sec)
    parse_compact_unwind(ctx, *unwind_sec);
}

template <typename E>
void ObjectFile<E>::parse_sections(Context<E> &ctx) {
  MachHeader &hdr = *(MachHeader *)this->mf->data;
  u8 *p = this->mf->data + sizeof(hdr);

  // Read all but a symtab section
  for (i64 i = 0; i < hdr.ncmds; i++) {
    LoadCommand &lc = *(LoadCommand *)p;
    p += lc.cmdsize;
    if (lc.cmd != LC_SEGMENT_64)
      continue;

    SegmentCommand &cmd = *(SegmentCommand *)&lc;
    MachSection *mach_sec = (MachSection *)((u8 *)&lc + sizeof(cmd));

    for (MachSection &msec : std::span(mach_sec, mach_sec + cmd.nsects)) {
      sections.push_back(nullptr);

      if (msec.match("__LD", "__compact_unwind")) {
        unwind_sec = &msec;
        continue;
      }

      if (msec.attr & S_ATTR_DEBUG)
        continue;

      sections.back().reset(new InputSection<E>(ctx, *this, msec));
    }
  }
}

template <typename E>
void ObjectFile<E>::parse_symbols(Context<E> &ctx) {
  SymtabCommand *cmd = (SymtabCommand *)find_load_command(ctx, LC_SYMTAB);
  if (!cmd)
    return;

  mach_syms = {(MachSym *)(this->mf->data + cmd->symoff), cmd->nsyms};
  this->syms.reserve(mach_syms.size());

  i64 nlocal = 0;
  for (MachSym &msym : mach_syms)
    if (!msym.is_extern)
      nlocal++;
  local_syms.reserve(nlocal);

  for (i64 i = 0; i < mach_syms.size(); i++) {
    MachSym &msym = mach_syms[i];
    std::string_view name = (char *)(this->mf->data + cmd->stroff + msym.stroff);

    if (msym.is_extern) {
      this->syms.push_back(get_symbol(ctx, name));
    } else {
      local_syms.emplace_back(name);
      Symbol<E> &sym = local_syms.back();

      sym.file = this;
      sym.subsec = nullptr;
      sym.scope = SCOPE_LOCAL;
      sym.is_common = false;
      sym.is_weak = false;

      switch (msym.type) {
      case N_UNDF:
        Fatal(ctx) << sym << ": local undef symbol?";
      case N_ABS:
        sym.value = msym.value;
        break;
      case N_SECT:
        // `value` and `subsec` will be filled by split_subsections
        break;
      default:
        Fatal(ctx) << *this << ": unknown symbol type for "
                   << sym << ": " << (u64)msym.type;
      }

      this->syms.push_back(&sym);
    }
  }
}

struct SplitRegion {
  u32 offset;
  u32 size;
  u32 symidx;
  bool is_alt_entry;
};

template <typename E>
struct SplitInfo {
  InputSection<E> *isec;
  std::vector<SplitRegion> regions;
};

template <typename E>
static std::vector<SplitInfo<E>>
split_regular_sections(Context<E> &ctx, ObjectFile<E> &file) {
  std::vector<SplitInfo<E>> vec(file.sections.size());

  for (i64 i = 0; i < file.sections.size(); i++)
    if (InputSection<E> *isec = file.sections[i].get())
      if (!isec->hdr.match("__TEXT", "__cstring"))
        vec[i].isec = isec;

  // Find all symbols whose type is N_SECT.
  for (i64 i = 0; i < file.mach_syms.size(); i++) {
    MachSym &msym = file.mach_syms[i];
    if (msym.type == N_SECT && vec[msym.sect - 1].isec) {
      SplitRegion r;
      r.offset = msym.value - vec[msym.sect - 1].isec->hdr.addr;
      r.symidx = i;
      r.is_alt_entry = (msym.desc & N_ALT_ENTRY);
      vec[msym.sect - 1].regions.push_back(r);
    }
  }

  std::erase_if(vec, [](const SplitInfo<E> &info) { return !info.isec; });

  sort(vec, [](const SplitInfo<E> &a, const SplitInfo<E> &b) {
    return a.isec->hdr.addr < b.isec->hdr.addr;
  });

  // Fix regions so that they cover the entire section without overlapping.
  for (SplitInfo<E> &info : vec) {
    std::vector<SplitRegion> &r = info.regions;

    if (r.empty()) {
      r.push_back({0, (u32)info.isec->hdr.size, (u32)-1, false});
      continue;
    }

    sort(r, [](const SplitRegion &a, const SplitRegion &b) {
      return a.offset < b.offset;
    });

    if (r[0].offset > 0)
      r.insert(r.begin(), {0, r[0].offset, (u32)-1, false});

    for (i64 i = 1; i < r.size(); i++)
      if (r[i - 1].offset == r[i].offset)
        r[i++].is_alt_entry = true;

    i64 last = -1;
    for (i64 i = 0; i < r.size(); i++) {
      if (!r[i].is_alt_entry) {
        if (last != -1)
          r[last].size = r[i].offset - r[last].offset;
        last = i;
      }
    }

    if (last != -1)
      r[last].size = info.isec->hdr.size - r[last].offset;
  }
  return vec;
}

template <typename E>
void ObjectFile<E>::split_subsections_via_symbols(Context<E> &ctx) {
  sym_to_subsec.resize(mach_syms.size());

  auto add = [&](InputSection<E> &isec, u32 offset, u32 size, u8 p2align) {
    Subsection<E> *subsec = new Subsection<E>{
      .isec = isec,
      .input_offset = offset,
      .input_size = size,
      .input_addr = (u32)(isec.hdr.addr + offset),
      .p2align = p2align,
    };

    subsec_pool.emplace_back(subsec);
    subsections.push_back(subsec);
  };

  // Split regular sections into subsections.
  for (SplitInfo<E> &info : split_regular_sections(ctx, *this)) {
    InputSection<E> &isec = *info.isec;
    for (SplitRegion &r : info.regions) {
      if (!r.is_alt_entry)
        add(isec, r.offset, r.size, isec.hdr.p2align);
      if (r.symidx != -1)
        sym_to_subsec[r.symidx] = subsections.back();
    }
  }

  // Split __cstring section.
  for (std::unique_ptr<InputSection<E>> &isec : sections) {
    if (isec && isec->hdr.match("__TEXT", "__cstring")) {
      std::string_view str = isec->contents;
      size_t pos = 0;

      while (pos < str.size()) {
        size_t end = str.find('\0', pos);
        if (end == str.npos)
          Fatal(ctx) << *this << " corruupted __TEXT,__cstring";

        end = str.find_first_not_of('\0', end);
        if (end == str.npos)
          end = str.size();

        // A constant string in __cstring has no alignment info, so we
        // need to infer it.
        u8 p2align = std::min<u8>(isec->hdr.p2align, std::countr_zero(pos));
        add(*isec, pos, end - pos, p2align);
        pos = end;
      }
    }
  }
}

template <typename E>
void ObjectFile<E>::init_subsections(Context<E> &ctx) {
  subsections.resize(sections.size());

  for (i64 i = 0; i < sections.size(); i++) {
    if (InputSection<E> *isec = sections[i].get()) {
      Subsection<E> *subsec = new Subsection<E>{
        .isec = *isec,
        .input_offset = 0,
        .input_size = (u32)isec->hdr.size,
        .input_addr = (u32)isec->hdr.addr,
        .p2align = (u8)isec->hdr.p2align,
      };
      subsec_pool.emplace_back(subsec);
      subsections[i] = subsec;
    }
  }

  sym_to_subsec.resize(mach_syms.size());

  for (i64 i = 0; i < mach_syms.size(); i++) {
    MachSym &msym = mach_syms[i];
    if (msym.type == N_SECT)
      sym_to_subsec[i] = subsections[msym.sect - 1];
  }

  std::erase(subsections, nullptr);
}

// Fix local symbols `subsec` members.
template <typename E>
void ObjectFile<E>::fix_subsec_members(Context<E> &ctx) {
  for (i64 i = 0; i < mach_syms.size(); i++) {
    MachSym &msym = mach_syms[i];
    Symbol<E> &sym = *this->syms[i];

    if (!msym.is_extern && msym.type == N_SECT) {
      Subsection<E> *subsec = sym_to_subsec[i];
      if (!subsec)
        subsec = find_subsection(ctx, msym.value);

      if (subsec) {
        sym.subsec = subsec;
        sym.value = msym.value - subsec->input_addr;
      } else {
        // Subsec is null if a symbol is in a __compact_unwind.
        sym.subsec = nullptr;
        sym.value = msym.value;
      }
    }
  }
}

template <typename E>
void ObjectFile<E>::parse_data_in_code(Context<E> &ctx) {
  if (auto *cmd = (LinkEditDataCommand *)find_load_command(ctx, LC_DATA_IN_CODE)) {
    data_in_code_entries = {
      (DataInCodeEntry *)(this->mf->data + cmd->dataoff),
      cmd->datasize / sizeof(DataInCodeEntry),
    };
  }
}

template <typename E>
std::vector<std::string> ObjectFile<E>::get_linker_options(Context<E> &ctx) {
  if (get_file_type(this->mf) == FileType::LLVM_BITCODE)
    return {};

  auto *cmd = (LinkerOptionCommand *)find_load_command(ctx, LC_LINKER_OPTION);
  if (!cmd)
    return {};

  char *buf = (char *)cmd + sizeof(*cmd);
  std::vector<std::string> vec;
  for (i64 i = 0; i < cmd->count; i++) {
    vec.push_back(buf);
    buf += vec.back().size() + 1;
  }
  return vec;
}

template <typename E>
LoadCommand *ObjectFile<E>::find_load_command(Context<E> &ctx, u32 type) {
  MachHeader &hdr = *(MachHeader *)this->mf->data;
  u8 *p = this->mf->data + sizeof(hdr);

  for (i64 i = 0; i < hdr.ncmds; i++) {
    LoadCommand &lc = *(LoadCommand *)p;
    if (lc.cmd == type)
      return &lc;
    p += lc.cmdsize;
  }
  return nullptr;
}

template <typename E>
Subsection<E> *ObjectFile<E>::find_subsection(Context<E> &ctx, u32 addr) {
  auto it = std::upper_bound(subsections.begin(), subsections.end(), addr,
                             [&](u32 addr, Subsection<E> *subsec) {
    return addr < subsec->input_addr;
  });

  if (it == subsections.begin())
    return nullptr;
  return *(it - 1);
}

template <typename E>
Symbol<E> *ObjectFile<E>::find_symbol(Context<E> &ctx, u32 addr) {
  for (i64 i = 0; i < mach_syms.size(); i++)
    if (MachSym &msym = mach_syms[i]; msym.is_extern && msym.value == addr)
      return this->syms[i];
  return nullptr;
}

template <typename E>
void ObjectFile<E>::parse_compact_unwind(Context<E> &ctx, MachSection &hdr) {
  if (hdr.size % sizeof(CompactUnwindEntry))
    Fatal(ctx) << *this << ": invalid __compact_unwind section size";

  i64 num_entries = hdr.size / sizeof(CompactUnwindEntry);
  unwind_records.reserve(num_entries);

  CompactUnwindEntry *src = (CompactUnwindEntry *)(this->mf->data + hdr.offset);

  // Read compact unwind entries
  for (i64 i = 0; i < num_entries; i++)
    unwind_records.emplace_back(src[i].code_len, src[i].encoding);

  // Read relocations
  MachRel *mach_rels = (MachRel *)(this->mf->data + hdr.reloff);
  for (i64 i = 0; i < hdr.nreloc; i++) {
    MachRel &r = mach_rels[i];
    if (r.offset >= hdr.size)
      Fatal(ctx) << *this << ": relocation offset too large: " << i;

    i64 idx = r.offset / sizeof(CompactUnwindEntry);
    UnwindRecord<E> &dst = unwind_records[idx];

    auto error = [&] {
      Fatal(ctx) << *this << ": __compact_unwind: unsupported relocation: " << i;
    };

    if (r.is_pcrel || r.p2size != 3 || r.type)
      error();

    switch (r.offset % sizeof(CompactUnwindEntry)) {
    case offsetof(CompactUnwindEntry, code_start): {
      if (r.is_extern)
        error();

      Subsection<E> *target = find_subsection(ctx, src[idx].code_start);
      if (!target)
        error();
      dst.subsec = target;
      dst.offset = src[idx].code_start - target->input_addr;
      break;
    }
    case offsetof(CompactUnwindEntry, personality):
      if (r.is_extern) {
        dst.personality = this->syms[r.idx];
      } else {
        u32 addr = *(ul32 *)((u8 *)this->mf->data + hdr.offset + r.offset);
        dst.personality = find_symbol(ctx, addr);
        if (!dst.personality)
          Fatal(ctx) << *this << ": __compact_unwind: unsupported local "
                     << "personality reference: " << i;
      }
      break;
    case offsetof(CompactUnwindEntry, lsda): {
      if (r.is_extern)
        error();

      u32 addr = *(ul32 *)((u8 *)this->mf->data + hdr.offset + r.offset);
      Subsection<E> *target = find_subsection(ctx, addr);
      if (!target)
        error();
      dst.lsda = target;
      dst.lsda_offset = addr - target->input_addr;
      break;
    }
    default:
      error();
    }
  }

  for (i64 i = 0; i < num_entries; i++)
    if (!unwind_records[i].subsec)
      Fatal(ctx) << "__compact_unwind: missing relocation at " << i;

  // Sort unwind entries by offset
  sort(unwind_records, [](const UnwindRecord<E> &a, const UnwindRecord<E> &b) {
    return std::tuple(a.subsec->input_addr, a.offset) <
           std::tuple(b.subsec->input_addr, b.offset);
  });

  // Associate unwind entries to subsections
  for (i64 i = 0; i < num_entries;) {
    Subsection<E> &subsec = *unwind_records[i].subsec;
    subsec.unwind_offset = i;

    i64 j = i + 1;
    while (j < num_entries && unwind_records[j].subsec == &subsec)
      j++;
    subsec.nunwind = j - i;
    i = j;
  }
}

// Symbols with higher priorities overwrites symbols with lower priorities.
// Here is the list of priorities, from the highest to the lowest.
//
//  1. Strong defined symbol
//  2. Weak defined symbol
//  3. Strong defined symbol in a DSO/archive
//  4. Weak Defined symbol in a DSO/archive
//  5. Common symbol
//  6. Common symbol in an archive
//  7. Unclaimed (nonexistent) symbol
//
// Ties are broken by file priority.
template <typename E>
static u64 get_rank(InputFile<E> *file, bool is_common, bool is_weak) {
  if (is_common) {
    assert(!file->is_dylib);
    if (!file->is_alive)
      return (6 << 24) + file->priority;
    return (5 << 24) + file->priority;
  }

  if (file->is_dylib || !file->is_alive) {
    if (is_weak)
      return (4 << 24) + file->priority;
    return (3 << 24) + file->priority;
  }

  if (is_weak)
    return (2 << 24) + file->priority;
  return (1 << 24) + file->priority;
}

template <typename E>
static u64 get_rank(Symbol<E> &sym) {
  if (!sym.file)
    return 7 << 24;
  return get_rank(sym.file, sym.is_common, sym.is_weak);
}

template <typename E>
void ObjectFile<E>::resolve_symbols(Context<E> &ctx) {
  auto is_private_extern = [&](MachSym &msym) {
    return this->is_hidden || msym.is_private_extern ||
           ((msym.desc & N_WEAK_REF) && (msym.desc & N_WEAK_DEF));
  };

  auto merge_scope = [&](Symbol<E> &sym, MachSym &msym) {
    // If at least one symbol defines it as an EXTERN symbol,
    // the result is an EXTERN symbol instead of PRIVATE_EXTERN,
    // so that the symbol is exported.
    if (sym.scope == SCOPE_EXTERN)
      return SCOPE_EXTERN;
    return is_private_extern(msym) ? SCOPE_PRIVATE_EXTERN : SCOPE_EXTERN;
  };

  for (i64 i = 0; i < this->syms.size(); i++) {
    MachSym &msym = mach_syms[i];
    if (!msym.is_extern || msym.is_undef())
      continue;

    Symbol<E> &sym = *this->syms[i];
    std::scoped_lock lock(sym.mu);
    bool is_weak = (msym.desc & N_WEAK_DEF);

    sym.scope = merge_scope(sym, msym);

    if (get_rank(this, msym.is_common(), is_weak) < get_rank(sym)) {
      sym.file = this;
      sym.is_imported = false;
      sym.is_weak = is_weak;

      switch (msym.type) {
      case N_UNDF:
        assert(msym.is_common());
        sym.subsec = nullptr;
        sym.value = msym.value;
        sym.is_common = true;
        break;
      case N_ABS:
        sym.subsec = nullptr;
        sym.value = msym.value;
        sym.is_common = false;
        break;
      case N_SECT:
        sym.subsec = sym_to_subsec[i];
        sym.value = msym.value - sym.subsec->input_addr;
        sym.is_common = false;
        break;
      default:
        Fatal(ctx) << sym << ": unknown symbol type: " << (u64)msym.type;
      }
    }
  }
}

template <typename E>
bool ObjectFile<E>::is_objc_object(Context<E> &ctx) {
  for (std::unique_ptr<InputSection<E>> &isec : sections)
    if (isec)
      if (isec->hdr.match("__DATA", "__objc_catlist") ||
          isec->hdr.match("__TEXT", "__swift"))
      return true;

  for (i64 i = 0; i < this->syms.size(); i++)
    if (!mach_syms[i].is_undef() && mach_syms[i].is_extern &&
        this->syms[i]->name.starts_with("_OBJC_CLASS_$_"))
      return true;
  return false;
}

template <typename E>
void
ObjectFile<E>::mark_live_objects(Context<E> &ctx,
                                 std::function<void(ObjectFile<E> *)> feeder) {
  assert(this->is_alive);

  for (i64 i = 0; i < this->syms.size(); i++) {
    MachSym &msym = mach_syms[i];
    if (!msym.is_extern)
      continue;

    Symbol<E> &sym = *this->syms[i];
    std::scoped_lock lock(sym.mu);
    if (!sym.file)
      continue;

    bool keep = msym.is_undef() || (msym.is_common() && !sym.is_common);
    if (keep && !sym.file->is_alive.exchange(true))
      if (!sym.file->is_dylib)
        feeder((ObjectFile<E> *)sym.file);
  }
}

template <typename E>
void ObjectFile<E>::convert_common_symbols(Context<E> &ctx) {
  for (i64 i = 0; i < this->mach_syms.size(); i++) {
    Symbol<E> &sym = *this->syms[i];
    MachSym &msym = mach_syms[i];

    if (sym.file == this && sym.is_common) {
      InputSection<E> *isec = get_common_sec(ctx);
      Subsection<E> *subsec = new Subsection<E>{
        .isec = *isec,
        .input_size = (u32)msym.value,
        .p2align = (u8)msym.p2align,
      };

      subsections.emplace_back(subsec);

      sym.is_imported = false;
      sym.is_weak = false;
      sym.subsec = subsec;
      sym.value = 0;
      sym.is_common = false;
    }
  }
}

template <typename E>
void ObjectFile<E>::check_duplicate_symbols(Context<E> &ctx) {
  for (i64 i = 0; i < this->mach_syms.size(); i++) {
    Symbol<E> *sym = this->syms[i];
    MachSym &msym = mach_syms[i];
    if (sym && sym->file && sym->file != this && !msym.is_undef() &&
        !msym.is_common() && !(msym.desc & N_WEAK_DEF))
      Error(ctx) << "duplicate symbol: " << *this << ": " << *sym->file
                 << ": " << *sym;
  }
}

template <typename E>
InputSection<E> *ObjectFile<E>::get_common_sec(Context<E> &ctx) {
  if (!common_sec) {
    MachSection *hdr = new MachSection;
    common_hdr.reset(hdr);

    memset(hdr, 0, sizeof(*hdr));
    hdr->set_segname("__DATA");
    hdr->set_sectname("__common");
    hdr->type = S_ZEROFILL;

    common_sec = new InputSection<E>(ctx, *this, *hdr);
    sections.emplace_back(common_sec);
  }
  return common_sec;
}

template <typename E>
void ObjectFile<E>::parse_lto_symbols(Context<E> &ctx) {
  i64 nsyms = ctx.lto.module_get_num_symbols(this->lto_module);
  this->syms.reserve(nsyms);
  this->mach_syms2.reserve(nsyms);

  for (i64 i = 0; i < nsyms; i++) {
    std::string_view name = ctx.lto.module_get_symbol_name(this->lto_module, i);
    this->syms.push_back(get_symbol(ctx, name));

    u32 attr = ctx.lto.module_get_symbol_attribute(this->lto_module, i);

    MachSym msym = {};
    msym.p2align = (attr & LTO_SYMBOL_ALIGNMENT_MASK);

    switch (attr & LTO_SYMBOL_DEFINITION_MASK) {
    case LTO_SYMBOL_DEFINITION_REGULAR:
    case LTO_SYMBOL_DEFINITION_TENTATIVE:
    case LTO_SYMBOL_DEFINITION_WEAK:
      msym.type = N_ABS;
      break;
    case LTO_SYMBOL_DEFINITION_UNDEFINED:
    case LTO_SYMBOL_DEFINITION_WEAKUNDEF:
      msym.type = N_UNDF;
      break;
    default:
      unreachable();
    }

    switch (attr & LTO_SYMBOL_SCOPE_MASK) {
    case 0:
    case LTO_SYMBOL_SCOPE_INTERNAL:
    case LTO_SYMBOL_SCOPE_HIDDEN:
      break;
    case LTO_SYMBOL_SCOPE_DEFAULT:
    case LTO_SYMBOL_SCOPE_PROTECTED:
    case LTO_SYMBOL_SCOPE_DEFAULT_CAN_BE_HIDDEN:
      msym.is_extern = true;
      break;
    default:
      unreachable();
    }

    mach_syms2.push_back(msym);
  }

  mach_syms = mach_syms2;
}

template <typename E>
static MappedFile<Context<E>> *
find_external_lib(Context<E> &ctx, std::string_view parent, std::string path) {
  if (!path.starts_with('/'))
    return MappedFile<Context<E>>::open(ctx, path);

  for (const std::string &root : ctx.arg.syslibroot) {
    if (path.ends_with(".tbd")) {
      if (auto *file = MappedFile<Context<E>>::open(ctx, root + path))
        return file;
      continue;
    }

    if (path.ends_with(".dylib")) {
      std::string stem(path.substr(0, path.size() - 6));
      if (auto *file = MappedFile<Context<E>>::open(ctx, root + stem + ".tbd"))
        return file;
      if (auto *file = MappedFile<Context<E>>::open(ctx, root + path))
        return file;
    }

    for (std::string extn : {".tbd", ".dylib"})
      if (auto *file = MappedFile<Context<E>>::open(ctx, root + path + extn))
        return file;
  }

  return nullptr;
}

template <typename E>
DylibFile<E> *DylibFile<E>::create(Context<E> &ctx, MappedFile<Context<E>> *mf) {
  DylibFile<E> *dylib = new DylibFile<E>(mf);
  dylib->is_alive = (ctx.needed_l || !ctx.arg.dead_strip_dylibs);
  dylib->is_weak = ctx.weak_l;
  dylib->is_reexported = ctx.reexport_l;
  ctx.dylib_pool.emplace_back(dylib);

  switch (get_file_type(mf)) {
  case FileType::TAPI:
    dylib->parse_tapi(ctx);
    break;
  case FileType::MACH_DYLIB:
    dylib->parse_dylib(ctx);
    break;
  default:
    Fatal(ctx) << mf->name << ": is not a dylib";
  }

  for (std::string_view path : dylib->reexported_libs) {
    MappedFile<Context<E>> *mf =
      find_external_lib(ctx, dylib->install_name, std::string(path));
    if (!mf)
      Fatal(ctx) << dylib->install_name << ": cannot open reexported library "
                 << path;

    DylibFile<E> *child = DylibFile<E>::create(ctx, mf);
    dylib->exports.merge(child->exports);
    dylib->weak_exports.merge(child->weak_exports);
  }

  for (std::string_view s : dylib->exports) {
    dylib->syms.push_back(get_symbol(ctx, s));
    dylib->is_weak_symbol.push_back(false);
  }

  for (std::string_view s : dylib->weak_exports) {
    if (!dylib->exports.contains(s)) {
      dylib->syms.push_back(get_symbol(ctx, s));
      dylib->is_weak_symbol.push_back(true);
    }
  }

  return dylib;
}

template <typename E>
void DylibFile<E>::read_trie(Context<E> &ctx, u8 *start, i64 offset,
                             const std::string &prefix) {
  u8 *buf = start + offset;

  if (*buf) {
    read_uleb(buf); // size
    i64 flags = read_uleb(buf);
    read_uleb(buf); // addr

    if (flags == EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION)
      weak_exports.insert(save_string(ctx, prefix));
    else
      exports.insert(save_string(ctx, prefix));
  } else {
    buf++;
  }

  i64 nchild = *buf++;

  for (i64 i = 0; i < nchild; i++) {
    std::string suffix((char *)buf);
    buf += suffix.size() + 1;
    i64 off = read_uleb(buf);
    read_trie(ctx, start, off, prefix + suffix);
  }
}

template <typename E>
void DylibFile<E>::parse_tapi(Context<E> &ctx) {
  TextDylib tbd = parse_tbd(ctx, this->mf);

  install_name = tbd.install_name;
  reexported_libs = std::move(tbd.reexported_libs);
  exports = std::move(tbd.exports);
  weak_exports = std::move(tbd.weak_exports);
}

template <typename E>
void DylibFile<E>::parse_dylib(Context<E> &ctx) {
  MachHeader &hdr = *(MachHeader *)this->mf->data;
  u8 *p = this->mf->data + sizeof(hdr);

  for (i64 i = 0; i < hdr.ncmds; i++) {
    LoadCommand &lc = *(LoadCommand *)p;

    switch (lc.cmd) {
    case LC_ID_DYLIB: {
      DylibCommand &cmd = *(DylibCommand *)p;
      install_name = (char *)p + cmd.nameoff;
      break;
    }
    case LC_DYLD_INFO_ONLY: {
      DyldInfoCommand &cmd = *(DyldInfoCommand *)p;
      if (cmd.export_off)
        read_trie(ctx, this->mf->data + cmd.export_off);
      break;
    }
    case LC_DYLD_EXPORTS_TRIE: {
      LinkEditDataCommand &cmd = *(LinkEditDataCommand *)p;
      read_trie(ctx, this->mf->data + cmd.dataoff);
      break;
    }
    case LC_REEXPORT_DYLIB: {
      DylibCommand &cmd = *(DylibCommand *)p;
      reexported_libs.push_back((char *)p + cmd.nameoff);
      break;
    }
    }
    p += lc.cmdsize;
  }
}

template <typename E>
void DylibFile<E>::resolve_symbols(Context<E> &ctx) {
  for (i64 i = 0; i < this->syms.size(); i++) {
    Symbol<E> &sym = *this->syms[i];
    std::scoped_lock lock(sym.mu);

    if (get_rank(this, false, false) < get_rank(sym)) {
      sym.file = this;
      sym.scope = SCOPE_LOCAL;
      sym.is_imported = true;
      sym.is_weak = (this->is_weak || is_weak_symbol[i]);
      sym.subsec = nullptr;
      sym.value = 0;
      sym.is_common = false;
    }
  }
}

#define INSTANTIATE(E)                                                  \
  template class InputFile<E>;                                          \
  template class ObjectFile<E>;                                         \
  template class DylibFile<E>;                                          \
  template std::ostream &operator<<(std::ostream &, const InputFile<E> &)

INSTANTIATE_ALL;

} // namespace mold::macho
