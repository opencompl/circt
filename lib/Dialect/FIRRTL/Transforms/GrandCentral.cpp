//===- GrandCentral.cpp - Ingest black box sources --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//
//
// Implement SiFive's Grand Central transform.  Currently, this supports
// SystemVerilog Interface generation.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAttributes.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"
#include <variant>

#define DEBUG_TYPE "gct"

using namespace circt;
using namespace firrtl;
using llvm::Optional;

//===----------------------------------------------------------------------===//
// Pass Implementation
//===----------------------------------------------------------------------===//

namespace {
/// A wrapper around a string that is used to encode a type which cannot be
/// represented by an mlir::Type for some reason.  This is currently used to
/// represent either an interface, a n-dimensional vector of interfaces, or a
/// tombstone for an actually unsupported type (e.g., an AugmentedBooleanType).
struct VerbatimType {
  /// The textual representation of the type.
  std::string str;

  /// True if this is a type which must be "instatiated" and requires a trailing
  /// "()".
  bool instantiation;

  /// Serialize this type to a string.
  std::string toStr() {
    return (Twine(str) + (instantiation ? "()" : "") + ";").str();
  }
};

/// A sum type representing either a type encoded as a string (VerbatimType)
/// or an actual mlir::Type.
typedef std::variant<VerbatimType, Type> TypeSum;

/// A namespace that is used to store existing names and generate names.  This
/// exists to work around limitations of SymbolTables.
class CircuitNamespace {
  StringSet<> internal;

public:
  /// Construct a new namespace from a circuit op.  This namespace will be
  /// composed of any operation in the first level of the circuit that contains
  /// a symbol.
  CircuitNamespace(CircuitOp circuit) {
    for (Operation &op : *circuit.getBody())
      if (auto symbol =
              op.getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName()))
        internal.insert(symbol.getValue());
  }

  /// Return a unique name, derived from the input `name`, and add the new name
  /// to the internal namespace.  There are two possible outcomes for the
  /// returned name:
  ///
  /// 1. The original name is returned.
  /// 2. The name is given a `_<n>` suffix where `<n>` is a number starting from
  ///    `_0` and incrementing by one each time.
  std::string newName(StringRef name) {
    // Special case the situation where there is no name collision to avoid
    // messing with the SmallString allocation below.
    if (internal.insert(name).second)
      return name.str();
    size_t i = 0;
    llvm::SmallString<64> tryName;
    do {
      tryName = (name + "_" + Twine(i++)).str();
    } while (!internal.insert(tryName).second);
    return std::string(tryName);
  }

  /// Return a unique name, derived from the input `name`, and add the new name
  /// to the internal namespace.  There are two possible outcomes for the
  /// returned name:
  ///
  /// 1. The original name is returned.
  /// 2. The name is given a `_<n>` suffix where `<n>` is a number starting from
  ///    `_0` and incrementing by one each time.
  std::string newName(const Twine &name) {
    return newName((StringRef)name.str());
  }
};

/// Stores the information content of an ExtractGrandCentralAnnotation.
struct ExtractionInfo {
  /// The directory where Grand Central generated collateral (modules,
  /// interfaces, etc.) will be written.
  StringAttr directory = {};

  /// The name of the file where any binds will be written.  This will be placed
  /// in the same output area as normal compilation output, e.g., output
  /// Verilog.  This has no relation to the `directory` member.
  StringAttr bindFilename = {};
};

/// Stores information about the companion module of a GrandCentral view.
struct CompanionInfo {
  StringRef name;

  FModuleOp companion;

  FModuleOp mapping;
};

/// Generate SystemVerilog interfaces from Grand Central annotations.  This pass
/// roughly works in the following three phases:
///
/// 1. Extraction information is determined.
///
/// 2. The circuit is walked to find all scattered annotations related to Grand
///    Central interfaces.  These are: (a) the parent module, (b) the companion
///    module, and (c) all leaves that are to be connected to the interface.
///
/// 3. The circuit-level Grand Central annotation is walked to both generate and
///    instantiate interfaces and to generate the "mappings" file that produces
///    cross-module references (XMRs) to drive the interface.
struct GrandCentralPass : public GrandCentralBase<GrandCentralPass> {
  void runOnOperation() override;

private:
  /// Optionally build an AugmentedType from an attribute.  Return none if the
  /// attribute is not a dictionary or if it does not match any of the known
  /// templates for AugmentedTypes.
  Optional<Attribute> fromAttr(Attribute attr);

  /// Mapping of ID to leaf ground type associated with that ID.
  DenseMap<Attribute, Value> leafMap;

  /// Mapping of ID to parent instance and module.
  DenseMap<Attribute, std::pair<InstanceOp, FModuleOp>> parentIDMap;

  /// Mapping of ID to companion module.
  DenseMap<Attribute, CompanionInfo> companionIDMap;

  /// Recursively examine an AugmentedType to populate the "mappings" file
  /// (generate XMRs) for this interface.  This does not build new interfaces.
  bool traverseField(Attribute field, IntegerAttr id, Twine path);

  /// Recursively examine an AugmentedType to both build new interfaces and
  /// populate a "mappings" file (generate XMRs) using `traverseField`.  Return
  /// the type of the field exmained.
  Optional<TypeSum> computeField(Attribute field, IntegerAttr id, Twine path);

  /// Recursively examine an AugmentedBundleType to both build new interfaces
  /// and populate a "mappings" file (generate XMRs).  Return none if the
  /// interface is invalid.
  Optional<sv::InterfaceOp> traverseBundle(AugmentedBundleTypeAttr bundle,
                                           IntegerAttr id, Twine path);

  /// Return the module associated with this value.
  FModuleLike getEnclosingModule(Value value);

  /// Inforamtion about how the circuit should be extracted.  This will be
  /// non-empty if an extraction annotation is found.
  Optional<ExtractionInfo> maybeExtractInfo = None;

  StringAttr getOutputDirectory() {
    if (maybeExtractInfo.hasValue())
      return maybeExtractInfo.getValue().directory;
    return {};
  }

  /// Store of an instance paths analysis.  This is constructed inside
  /// `runOnOperation`, to work around the deleted copy constructor of
  /// `InstancePathCache`'s internal `BumpPtrAllocator`.
  ///
  /// TODO: Investigate a way to not use a pointer here like how `getNamespace`
  /// works below.
  InstancePathCache *instancePaths = nullptr;

  /// The namespace associated with the circuit.  This is lazily constructed
  /// using `getNamesapce`.
  Optional<CircuitNamespace> circuitNamespace = None;

  /// Return a reference to the circuit namespace.  This will lazily construct a
  /// namespace if one does not exist.
  CircuitNamespace &getNamespace() {
    if (!circuitNamespace)
      circuitNamespace = CircuitNamespace(getOperation());
    return circuitNamespace.getValue();
  }

  /// A symbol table associated with the circuit.  This is lazily constructed by
  /// `getSymbolTable`.
  Optional<SymbolTable> symbolTable = None;

  /// Return a reference to a circuit-level symbol table.  Lazily construct one
  /// if such a symbol table does not already exist.
  SymbolTable &getSymbolTable() {
    if (!symbolTable)
      symbolTable = SymbolTable(getOperation());
    return symbolTable.getValue();
  }

  // Utility that acts like emitOpError, but does _not_ include a note.  The
  // note in emitOpError includes the entire op which means the **ENTIRE**
  // FIRRTL circuit.  This doesn't communicate anything useful to the user
  // other than flooding their terminal.
  InFlightDiagnostic emitCircuitError(StringRef message = {}) {
    return emitError(getOperation().getLoc(), "'firrtl.circuit' op " + message);
  }
};

} // namespace

Optional<Attribute> GrandCentralPass::fromAttr(Attribute attr) {
  auto dict = attr.dyn_cast<DictionaryAttr>();
  if (!dict) {
    emitCircuitError() << "attribute is not a dictionary: " << attr << "\n";
    return None;
  }

  auto clazz = dict.getAs<StringAttr>("class");
  if (!clazz) {
    emitCircuitError() << "missing 'class' key in " << dict << "\n";
    return None;
  }

  auto classBase = clazz.getValue();
  classBase.consume_front("sifive.enterprise.grandcentral.Augmented");

  if (classBase == "BundleType") {
    if (dict.getAs<StringAttr>("defName") && dict.getAs<ArrayAttr>("elements"))
      return AugmentedBundleTypeAttr::get(&getContext(), dict);
    emitCircuitError() << "has an invalid AugmentedBundleType that does not "
                          "contain 'defName' and 'elements' fields: "
                       << dict;
  } else if (classBase == "VectorType") {
    if (dict.getAs<StringAttr>("name") && dict.getAs<ArrayAttr>("elements"))
      return AugmentedVectorTypeAttr::get(&getContext(), dict);
    emitCircuitError() << "has an invalid AugmentedVectorType that does not "
                          "contain 'name' and 'elements' fields: "
                       << dict;
  } else if (classBase == "GroundType") {
    auto id = dict.getAs<IntegerAttr>("id");
    auto name = dict.getAs<StringAttr>("name");
    if (id && leafMap.count(id) && name)
      return AugmentedGroundTypeAttr::get(&getContext(), dict);
    if (!id || !name)
      emitCircuitError() << "has an invalid AugmentedGroundType that does not "
                            "contain 'id' and 'name' fields:  "
                         << dict;
    if (id && !leafMap.count(id))
      emitCircuitError() << "has an AugmentedGroundType with 'id == "
                         << id.getValue().getZExtValue()
                         << "' that does not have a scattered leaf to connect "
                            "to in the circuit "
                            "(was the leaf deleted or constant prop'd away?)";
  } else if (classBase == "StringType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedStringTypeAttr::get(&getContext(), dict);
  } else if (classBase == "BooleanType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedBooleanTypeAttr::get(&getContext(), dict);
  } else if (classBase == "IntegerType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedIntegerTypeAttr::get(&getContext(), dict);
  } else if (classBase == "DoubleType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedDoubleTypeAttr::get(&getContext(), dict);
  } else if (classBase == "LiteralType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedLiteralTypeAttr::get(&getContext(), dict);
  } else if (classBase == "DeletedType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedDeletedTypeAttr::get(&getContext(), dict);
  } else {
    emitCircuitError() << "has an invalid AugmentedType";
  }
  return None;
}

bool GrandCentralPass::traverseField(Attribute field, IntegerAttr id,
                                     Twine path) {
  return TypeSwitch<Attribute, bool>(field)
      .Case<AugmentedGroundTypeAttr>([&](auto ground) {
        Value leafValue = leafMap.lookup(ground.getID());
        assert(leafValue && "leafValue not found");

        auto builder = OpBuilder::atBlockEnd(
            companionIDMap.lookup(id).mapping.getBodyBlock());

        auto srcPaths =
            instancePaths->getAbsolutePaths(getEnclosingModule(leafValue));
        assert(srcPaths.size() == 1 &&
               "Unable to handle multiply instantiated companions");
        llvm::SmallString<128> srcRef;
        for (auto path : srcPaths[0]) {
          srcRef.append(path.name());
          srcRef.append(".");
        }

        auto uloc = builder.getUnknownLoc();
        if (auto blockArg = leafValue.dyn_cast<BlockArgument>()) {
          FModuleOp module =
              cast<FModuleOp>(blockArg.getOwner()->getParentOp());
          builder.create<sv::VerbatimOp>(
              uloc, "assign " + path + " = " + srcRef +
                        module.portNames()[blockArg.getArgNumber()]
                            .cast<StringAttr>()
                            .getValue() +
                        ";");
        } else {
          auto leafModuleName = leafValue.getDefiningOp()
                                    ->getAttr("name")
                                    .cast<StringAttr>()
                                    .getValue();
          builder.create<sv::VerbatimOp>(
              uloc, "assign " + path + " = " + srcRef + leafModuleName + ";");
        }
        return true;
      })
      .Case<AugmentedVectorTypeAttr>([&](auto vector) {
        bool notFailed = true;
        auto elements = vector.getElements();
        for (size_t i = 0, e = elements.size(); i != e; ++i) {
          auto field = fromAttr(elements[i]);
          if (!field)
            return false;
          notFailed &=
              traverseField(field.getValue(), id, path + "[" + Twine(i) + "]");
        }
        return notFailed;
      })
      .Case<AugmentedBundleTypeAttr>([&](AugmentedBundleTypeAttr bundle) {
        bool anyFailed = true;
        for (auto element : bundle.getElements()) {
          auto field = fromAttr(element);
          if (!field)
            return false;
          auto name = element.cast<DictionaryAttr>().getAs<StringAttr>("name");
          if (!name)
            name = element.cast<DictionaryAttr>().getAs<StringAttr>("defName");
          anyFailed &=
              traverseField(field.getValue(), id, path + "." + name.getValue());
        }

        return anyFailed;
      })
      .Case<AugmentedStringTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedBooleanTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedIntegerTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedDoubleTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedLiteralTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedDeletedTypeAttr>([&](auto a) { return false; })
      .Default([](auto a) { return true; });
}

Optional<TypeSum> GrandCentralPass::computeField(Attribute field,
                                                 IntegerAttr id, Twine path) {

  auto unsupported = [&](StringRef name, StringRef kind) {
    return VerbatimType(
        {("// " + name + " = <unsupported " + kind + " type>").str(), false});
  };

  return TypeSwitch<Attribute, Optional<TypeSum>>(field)
      .Case<AugmentedGroundTypeAttr>(
          [&](AugmentedGroundTypeAttr ground) -> Optional<TypeSum> {
            // Traverse to generate mappings.
            traverseField(field, id, path);
            auto value = leafMap.lookup(ground.getID());
            auto tpe = value.getType().cast<FIRRTLType>();
            if (!tpe.isGround()) {
              value.getDefiningOp()->emitOpError()
                  << "cannot be added to interface with id '"
                  << id.getValue().getZExtValue()
                  << "' because it is not a ground type";
              return None;
            }
            return TypeSum(IntegerType::get(getOperation().getContext(),
                                            tpe.getBitWidthOrSentinel()));
          })
      .Case<AugmentedVectorTypeAttr>(
          [&](AugmentedVectorTypeAttr vector) -> Optional<TypeSum> {
            bool notFailed = true;
            auto elements = vector.getElements();
            auto firstElement = fromAttr(elements[0]);
            auto elementType = computeField(firstElement.getValue(), id,
                                            path + "[" + Twine(0) + "]");
            if (!elementType)
              return None;

            for (size_t i = 1, e = elements.size(); i != e; ++i) {
              auto subField = fromAttr(elements[i]);
              if (!subField)
                return None;
              notFailed &= traverseField(subField.getValue(), id,
                                         path + "[" + Twine(i) + "]");
            }

            if (auto *tpe = std::get_if<Type>(&elementType.getValue()))
              return TypeSum(
                  hw::UnpackedArrayType::get(*tpe, elements.getValue().size()));
            auto str = std::get<VerbatimType>(elementType.getValue());
            str.str.append(
                ("[" + Twine(elements.getValue().size()) + "]").str());
            return TypeSum(str);
          })
      .Case<AugmentedBundleTypeAttr>(
          [&](AugmentedBundleTypeAttr bundle) -> TypeSum {
            auto iface = traverseBundle(bundle, id, path);
            assert(iface && iface.getValue());
            return VerbatimType({(iface.getValue().getName() + " " +
                                  bundle.getDefName().getValue())
                                     .str(),
                                 true});
          })
      .Case<AugmentedStringTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "string");
      })
      .Case<AugmentedBooleanTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "boolean");
      })
      .Case<AugmentedIntegerTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "integer");
      })
      .Case<AugmentedDoubleTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "double");
      })
      .Case<AugmentedLiteralTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "literal");
      })
      .Case<AugmentedDeletedTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "deleted");
      });
}

/// Traverse an Annotation that is an AugmentedBundleType.  During
/// traversal, construct any discovered SystemVerilog interfaces.  If this
/// is the root interface, instantiate that interface in the parent. Recurse
/// into fields of the AugmentedBundleType to construct nested interfaces
/// and generate stringy-typed SystemVerilog hierarchical references to
/// drive the interface. Returns false on any failure and true on success.
Optional<sv::InterfaceOp>
GrandCentralPass::traverseBundle(AugmentedBundleTypeAttr bundle, IntegerAttr id,
                                 Twine path) {
  auto builder = OpBuilder::atBlockEnd(getOperation().getBody());
  sv::InterfaceOp iface;
  builder.setInsertionPointToEnd(getOperation().getBody());
  auto loc = getOperation().getLoc();
  auto iFaceName = getNamespace().newName(bundle.getDefName().getValue());
  iface = builder.create<sv::InterfaceOp>(loc, iFaceName);
  iface->setAttr(
      "output_file",
      hw::OutputFileAttr::get(getOutputDirectory(),
                              builder.getStringAttr(iFaceName + ".sv"),
                              builder.getBoolAttr(true),
                              builder.getBoolAttr(true), builder.getContext()));

  builder.setInsertionPointToEnd(cast<sv::InterfaceOp>(iface).getBodyBlock());

  for (auto element : bundle.getElements()) {
    auto field = fromAttr(element);
    if (!field)
      return None;

    auto name = element.cast<DictionaryAttr>().getAs<StringAttr>("name");
    if (!name)
      name = element.cast<DictionaryAttr>().getAs<StringAttr>("defName");
    auto elementType =
        computeField(field.getValue(), id, path + "." + name.getValue());
    if (!elementType)
      return None;

    auto uloc = builder.getUnknownLoc();
    auto description =
        element.cast<DictionaryAttr>().getAs<StringAttr>("description");
    if (description)
      builder.create<sv::VerbatimOp>(uloc,
                                     ("// " + description.getValue()).str());
    if (auto *str = std::get_if<VerbatimType>(&elementType.getValue())) {
      builder.create<sv::VerbatimOp>(uloc, str->toStr());
      continue;
    }

    builder.create<sv::InterfaceSignalOp>(
        uloc, name.getValue(), std::get<Type>(elementType.getValue()));
  }

  return iface;
}

/// Return the module that is associated with this value.  Use the cached/lazily
/// constructed symbol table to make this fast.
FModuleLike GrandCentralPass::getEnclosingModule(Value value) {
  if (auto blockArg = value.dyn_cast<BlockArgument>())
    return cast<FModuleOp>(blockArg.getOwner()->getParentOp());

  auto *op = value.getDefiningOp();
  if (InstanceOp instance = dyn_cast<InstanceOp>(op))
    return getSymbolTable().lookup<FModuleOp>(
        instance.moduleNameAttr().getValue());

  return op->getParentOfType<FModuleOp>();
}

/// This method contains the business logic of this pass.
void GrandCentralPass::runOnOperation() {
  LLVM_DEBUG(llvm::dbgs() << "===- Running Grand Central Views/Interface Pass "
                             "-----------------------------===\n");

  CircuitOp circuitOp = getOperation();

  // Look at the circuit annotaitons to do two things:
  //
  // 1. Determine extraction information (directory and filename).
  // 2. Populate a worklist of all annotations that encode interfaces.
  //
  // Remove annotations encoding interfaces, but leave extraction information as
  // this may be needed by later passes.
  SmallVector<Annotation> worklist;
  bool removalError = false;
  AnnotationSet::removeAnnotations(circuitOp, [&](Annotation anno) {
    if (anno.isClass("sifive.enterprise.grandcentral.AugmentedBundleType")) {
      worklist.push_back(anno);
      return true;
    }
    if (anno.isClass(
            "sifive.enterprise.grandcentral.ExtractGrandCentralAnnotation")) {
      if (maybeExtractInfo.hasValue()) {
        emitCircuitError("more than one 'ExtractGrandCentralAnnotation' was "
                         "found, but exactly one must be provided");
        removalError = true;
        return false;
      }

      auto directory = anno.getMember<StringAttr>("directory");
      auto filename = anno.getMember<StringAttr>("filename");
      if (!directory || !filename) {
        emitCircuitError()
            << "contained an invalid 'ExtractGrandCentralAnnotation' that does "
               "not contain 'directory' and 'filename' fields: "
            << anno.getDict();
        removalError = true;
        return false;
      }

      maybeExtractInfo = {directory, filename};
      // Intentional fallthrough.  Extraction info may be needed later.
    }
    return false;
  });

  if (removalError)
    return signalPassFailure();

  // Exit immediately if no annotations indicative of interfaces that need to be
  // built exist.
  if (worklist.empty())
    return markAllAnalysesPreserved();

  LLVM_DEBUG({
    llvm::dbgs() << "Extraction Info:\n";
    if (maybeExtractInfo)
      llvm::dbgs() << "  directory: " << maybeExtractInfo.getValue().directory
                   << "\n"
                   << "  filename: " << maybeExtractInfo.getValue().bindFilename
                   << "\n";
    else
      llvm::dbgs() << "  <none>\n";
  });

  // Setup the builder to create ops _inside the FIRRTL circuit_.  This is
  // necessary because interfaces and interface instances are created.
  // Instances link to their definitions via symbols and we don't want to
  // break this.
  auto builder = OpBuilder::atBlockEnd(circuitOp.getBody());

  // Maybe get an "id" from an Annotation.  Generate error messages on the op if
  // no "id" exists.
  auto getID = [&](Operation *op,
                   Annotation annotation) -> Optional<IntegerAttr> {
    auto id = annotation.getMember<IntegerAttr>("id");
    if (!id) {
      op->emitOpError()
          << "contained a malformed "
             "'sifive.enterprise.grandcentral.AugmentedGroundType' annotation "
             "that did not contain an 'id' field";
      removalError = true;
      return None;
    }
    return Optional(id);
  };

  // Maybe return the lone instance of a module.  Generate errors on the op if
  // the module is not instantiated or is multiply instantiated.
  auto exactlyOneInstance = [&](FModuleOp op,
                                StringRef msg) -> Optional<InstanceOp> {
    auto instances = getSymbolTable().getSymbolUses(op, circuitOp);
    if (!instances.hasValue()) {
      op->emitOpError() << "is marked as a GrandCentral '" << msg
                        << "', but is never instantiated";
      return None;
    }

    if (llvm::hasSingleElement(instances.getValue()))
      return cast<InstanceOp>((*(instances.getValue().begin())).getUser());

    auto diag = op->emitOpError() << "is marked as a GrandCentral '" << msg
                                  << "', but it is instantiated more than once";
    for (auto instance : instances.getValue())
      diag.attachNote(instance.getUser()->getLoc())
          << "parent is instantiated here";
    return None;
  };

  /// Walk the circuit and extract all information related to scattered
  /// Grand Central annotations.  This is used to populate: (1) the
  /// companionIDMap, (2) the parentIDMap, and (3) the leafMap.
  /// Annotations are removed as they are discovered and if they are not
  /// malformed.
  removalError = false;
  auto trueAttr = builder.getBoolAttr(true);
  circuitOp.walk([&](Operation *op) {
    TypeSwitch<Operation *>(op)
        .Case<RegOp, RegResetOp, WireOp, NodeOp>([&](auto op) {
          AnnotationSet::removeAnnotations(op, [&](Annotation annotation) {
            if (!annotation.isClass(
                    "sifive.enterprise.grandcentral.AugmentedGroundType"))
              return false;
            auto maybeID = getID(op, annotation);
            if (!maybeID)
              return false;
            leafMap[maybeID.getValue()] = op.getResult();
            return true;
          });
        })
        // TODO: Figure out what to do with this.
        .Case<InstanceOp>([&](auto op) {
          AnnotationSet::removePortAnnotations(op, [&](unsigned i,
                                                       Annotation annotation) {
            if (!annotation.isClass(
                    "sifive.enterprise.grandcentral.AugmentedGroundType"))
              return false;
            op.emitOpError()
                << "is marked as an interface element, but this should be "
                   "impossible due to how the Chisel Grand Central API works";
            removalError = true;
            return false;
          });
        })
        .Case<MemOp>([&](auto op) {
          AnnotationSet::removeAnnotations(op, [&](Annotation annotation) {
            if (!annotation.isClass(
                    "sifive.enterprise.grandcentral.AugmentedGroundType"))
              return false;
            op.emitOpError()
                << "is marked as an interface element, but this does not make "
                   "sense (is there a scattering bug or do you have a "
                   "malformed hand-crafted MLIR circuit?)";
            removalError = true;
            return false;
          });
          AnnotationSet::removePortAnnotations(
              op, [&](unsigned i, Annotation annotation) {
                if (!annotation.isClass(
                        "sifive.enterprise.grandcentral.AugmentedGroundType"))
                  return false;
                op.emitOpError()
                    << "has port '" << i
                    << "' marked as an interface element, but this does not "
                       "make sense (is there a scattering bug or do you have a "
                       "malformed hand-crafted MLIR circuit?)";
                removalError = true;
                return false;
              });
        })
        .Case<FModuleOp>([&](FModuleOp op) {
          // Handle annotations on the ports.
          AnnotationSet::removePortAnnotations(
              op, [&](unsigned i, Annotation annotation) {
                if (!annotation.isClass(
                        "sifive.enterprise.grandcentral.AugmentedGroundType"))
                  return false;
                auto maybeID = getID(op, annotation);
                if (!maybeID)
                  return false;
                leafMap[maybeID.getValue()] = op.getArgument(i);

                return true;
              });

          // Handle annotations on the module.
          AnnotationSet::removeAnnotations(op, [&](Annotation annotation) {
            if (!annotation.isClass(
                    "sifive.enterprise.grandcentral.ViewAnnotation"))
              return false;
            auto tpe = annotation.getMember<StringAttr>("type");
            auto name = annotation.getMember<StringAttr>("name");
            auto id = annotation.getMember<IntegerAttr>("id");
            if (!tpe) {
              op.emitOpError()
                  << "has a malformed "
                     "'sifive.enterprise.grandcentral.ViewAnnotation' that did "
                     "not contain a 'type' field with a 'StringAttr' value";
              goto FModuleOp_error;
            }
            if (!id) {
              op.emitOpError()
                  << "has a malformed "
                     "'sifive.enterprise.grandcentral.ViewAnnotation' that did "
                     "not contain an 'id' field with an 'IntegerAttr' value";
              goto FModuleOp_error;
            }
            if (!name) {
              op.emitOpError()
                  << "has a malformed "
                     "'sifive.enterprise.grandcentral.ViewAnnotation' that did "
                     "not contain a 'name' field with a 'StringAttr' value";
              goto FModuleOp_error;
            }

            // If this is a companion, then:
            //   1. Insert it into the companion map
            //   2. Create a new mapping module.
            //   3. Instatiate the mapping module in the companion.
            //   4. Check that the companion is instantated exactly once.
            //   5. Set attributes on that lone instance so it will become a
            //      bind if extraction information was provided.
            if (tpe.getValue() == "companion") {
              builder.setInsertionPointToEnd(circuitOp.getBody());

              // Create the mapping module.
              auto mappingName =
                  getNamespace().newName(name.getValue() + "_mapping");
              auto mapping = builder.create<FModuleOp>(
                  circuitOp.getLoc(), builder.getStringAttr(mappingName),
                  ArrayRef<ModulePortInfo>());
              auto *ctx = builder.getContext();
              mapping->setAttr(
                  "output_file",
                  hw::OutputFileAttr::get(
                      getOutputDirectory(),
                      builder.getStringAttr(mapping.getName() + ".sv"),
                      trueAttr, trueAttr, ctx));
              companionIDMap[id] = {name.getValue(), op, mapping};

              // Instantiate the mapping module inside the companion.
              builder.setInsertionPointToEnd(op.getBodyBlock());
              builder.create<InstanceOp>(circuitOp.getLoc(),
                                         SmallVector<Type>({}),
                                         mapping.getName(), mapping.getName());

              // Assert that the companion is instantiated once and only once.
              auto instance = exactlyOneInstance(op, "companion");
              if (!instance)
                return false;

              // If no extraction info was provided, exit.  Otherwise, setup the
              // lone instance of the companion to be lowered as a bind.
              if (!maybeExtractInfo)
                return true;

              instance.getValue()->setAttr("lowerToBind", trueAttr);
              instance.getValue()->setAttr(
                  "output_file", hw::OutputFileAttr::get(
                                     builder.getStringAttr(""),
                                     maybeExtractInfo.getValue().bindFilename,
                                     trueAttr, trueAttr, ctx));
              op->setAttr("output_file",
                          hw::OutputFileAttr::get(
                              maybeExtractInfo.getValue().directory,
                              builder.getStringAttr(op.getName() + ".sv"),
                              trueAttr, trueAttr, ctx));
              return true;
            }

            // Insert the parent into the parent map, asserting that the parent
            // is instantiated exatly once.
            if (tpe.getValue() == "parent") {
              // Assert that the parent is instantiated once and only once.
              auto instance = exactlyOneInstance(op, "parent");
              if (!instance)
                return false;

              parentIDMap[id] = {instance.getValue(), cast<FModuleOp>(op)};
              return true;
            }

            op.emitOpError()
                << "has a 'sifive.enterprise.grandcentral.ViewAnnotation' with "
                   "an unknown or malformed 'type' field in annotation: "
                << annotation.getDict();

          FModuleOp_error:
            removalError = true;
            return false;
          });
        });
  });

  if (removalError)
    return signalPassFailure();

  // Check that a parent exists for every companion.
  for (auto a : companionIDMap) {
    if (parentIDMap.count(a.first) == 0) {
      emitCircuitError()
          << "contains a 'companion' with id '"
          << a.first.cast<IntegerAttr>().getValue().getZExtValue()
          << "', but does not contain a GrandCentral 'parent' with the same id";
      return signalPassFailure();
    }
  }

  // Check that a companion exists for every parent.
  for (auto a : parentIDMap) {
    if (companionIDMap.count(a.first) == 0) {
      emitCircuitError()
          << "contains a 'parent' with id '"
          << a.first.cast<IntegerAttr>().getValue().getZExtValue()
          << "', but does not contain a GrandCentral 'companion' with the same "
             "id";
      return signalPassFailure();
    }
  }

  LLVM_DEBUG({
    // Print out the companion map, parent map, and all leaf values that
    // were discovered.  Sort these by their keys before printing to make
    // this easier to read.
    SmallVector<IntegerAttr> ids;
    auto sort = [&ids]() {
      llvm::sort(ids, [](IntegerAttr a, IntegerAttr b) {
        return a.getValue().getZExtValue() < b.getValue().getZExtValue();
      });
    };
    for (auto tuple : companionIDMap)
      ids.push_back(tuple.first.cast<IntegerAttr>());
    sort();
    llvm::dbgs() << "companionIDMap:\n";
    for (auto id : ids) {
      auto value = companionIDMap.lookup(id);
      llvm::dbgs() << "  - " << id.getValue() << ": "
                   << value.companion.getName() << " -> " << value.name << "\n";
    }
    llvm::dbgs() << "parentIDMap:\n";
    for (auto id : ids) {
      auto value = parentIDMap.lookup(id);
      llvm::dbgs() << "  - " << id.getValue() << ": " << value.first.name()
                   << ":" << value.second.getName() << "\n";
    }
    ids.clear();
    for (auto tuple : leafMap)
      ids.push_back(tuple.first.cast<IntegerAttr>());
    sort();
    llvm::dbgs() << "leafMap:\n";
    for (auto id : ids) {
      auto value = leafMap.lookup(id);
      if (auto blockArg = value.dyn_cast<BlockArgument>()) {
        FModuleOp module = cast<FModuleOp>(blockArg.getOwner()->getParentOp());
        llvm::dbgs() << "  - " << id.getValue() << ": "
                     << module.getName() + ">" +
                            module.portNames()[blockArg.getArgNumber()]
                                .cast<StringAttr>()
                                .getValue()
                     << "\n";
      } else
        llvm::dbgs() << "  - " << id.getValue() << ": "
                     << value.getDefiningOp()
                            ->getAttr("name")
                            .cast<StringAttr>()
                            .getValue()
                     << "\n";
    }
  });

  /// TODO: Handle this differently to allow construction of an optionsl
  auto instancePathCache = InstancePathCache(getAnalysis<InstanceGraph>());
  instancePaths = &instancePathCache;

  // Now, iterate over the worklist of interface-encoding annotations to create
  // the interface and all its sub-interfaces (interfaces that it instantiates),
  // instantiate the top-level interface, and generate a "mappings file" that
  // will use XMRs to drive the interface.  If extraction info is available,
  // then the top-level instantiate interface will be marked for extraction via
  // a SystemVerilog bind.
  for (auto anno : worklist) {
    auto bundle = AugmentedBundleTypeAttr::get(&getContext(), anno.getDict());

    // The top-level AugmentedBundleType must have a global ID field so that
    // this can be linked to the parent and companion.
    if (!bundle.isRoot()) {
      emitCircuitError() << "missing 'id' in root-level BundleType: "
                         << anno.getDict() << "\n";
      removalError = true;
      continue;
    }

    // Error if a matching parent or companion do not exist.
    if (parentIDMap.count(bundle.getID()) == 0) {
      emitCircuitError() << "no parent found with 'id' value '"
                         << bundle.getID().getValue().getZExtValue() << "'\n";
      removalError = true;
      continue;
    }
    if (companionIDMap.count(bundle.getID()) == 0) {
      emitCircuitError() << "no companion found with 'id' value '"
                         << bundle.getID().getValue().getZExtValue() << "'\n";
      removalError = true;
      continue;
    }

    // Recursively walk the AugmentedBundleType to generate interfaces and XMRs.
    // Error out if this returns None (indicating that the annotation annotation
    // is malformed in some way).  A good error message is generated inside
    // `traverseBundle` or the functions it calls.
    auto iface = traverseBundle(bundle, bundle.getID(),
                                companionIDMap.lookup(bundle.getID()).name);
    if (!iface) {
      removalError = true;
      continue;
    }

    // Instantiate the interface inside the parent.
    builder.setInsertionPointToEnd(
        parentIDMap.lookup(bundle.getID()).second.getBodyBlock());
    auto symbolName = getNamespace().newName(
        "__" + companionIDMap.lookup(bundle.getID()).name + "_" +
        bundle.getDefName().getValue() + "__");
    auto instance = builder.create<sv::InterfaceInstanceOp>(
        getOperation().getLoc(), iface.getValue().getInterfaceType(),
        companionIDMap.lookup(bundle.getID()).name,
        builder.getStringAttr(symbolName));

    // If no extraction information was present, then just leave the interface
    // instantiated in the parent.  Otherwise, make it a bind.
    if (!maybeExtractInfo)
      continue;

    instance->setAttr("doNotPrint", trueAttr);
    builder.setInsertionPointToStart(
        instance->getParentOfType<ModuleOp>().getBody());
    auto bind = builder.create<sv::BindInterfaceOp>(
        getOperation().getLoc(),
        SymbolRefAttr::get(builder.getContext(),
                           instance.sym_name().getValue()));
    bind->setAttr("output_file", hw::OutputFileAttr::get(
                                     builder.getStringAttr(""),
                                     maybeExtractInfo.getValue().bindFilename,
                                     trueAttr, trueAttr, bind.getContext()));
  }

  // Signal pass failure if any errors were found while examining circuit
  // annotations.
  if (removalError)
    return signalPassFailure();
}

//===----------------------------------------------------------------------===//
// Pass Creation
//===----------------------------------------------------------------------===//

std::unique_ptr<mlir::Pass> circt::firrtl::createGrandCentralPass() {
  return std::make_unique<GrandCentralPass>();
}
