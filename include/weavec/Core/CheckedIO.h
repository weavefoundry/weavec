//===- CheckedIO.h - Bounded checked contract transport ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_CHECKEDIO_H
#define WEAVEC_CORE_CHECKEDIO_H

#include "weavec/Core/SummaryIO.h"

namespace weavec::core {

/// Single-token hex encoding of length-delimited fields, checked encoding 4
/// (summary format 16). The source strings are opaque data; no field is
/// executed or reparsed as code.
[[nodiscard]] std::string printCheckedContract(const CheckedContract &contract,
                                               const GlobalNamer &names);
[[nodiscard]] std::optional<CheckedContract>
parseCheckedContract(std::string_view record, const GlobalResolver &resolve);

} // namespace weavec::core

#endif // WEAVEC_CORE_CHECKEDIO_H
