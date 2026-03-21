// SPDX-FileCopyrightText: Deutsches Elektronen-Synchrotron DESY, MSK, ChimeraTK Project <chimeratk-support@desy.de>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "TypeChangingDecorator.h"

namespace ChimeraTK {

  // The global instance of the map
  std::map<DecoratorMapKey, boost::shared_ptr<ChimeraTK::TransferElement>> globalDecoratorMap;

  std::map<DecoratorMapKey, boost::shared_ptr<ChimeraTK::TransferElement>>& getGlobalDecoratorMap() {
    return globalDecoratorMap;
  }

  // the implementations of the full template specialisations
  namespace csa_helpers {

    template<>
    int8_t stringToT<int8_t>(std::string const& input) {
      return static_cast<int8_t>(std::stoi(input));
    }

    template<>
    uint8_t stringToT<uint8_t>(std::string const& input) {
      return static_cast<uint8_t>(std::stoul(input));
    }

    template<>
    std::string T_ToString<uint8_t>(uint8_t input) {
      return std::to_string(static_cast<uint32_t>(input));
    }

    template<>
    std::string T_ToString<int8_t>(int8_t input) {
      return std::to_string(static_cast<int32_t>(input));
    }
  } // namespace csa_helpers

} // namespace ChimeraTK
