//
// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
//

#ifndef VPUX_ELF_LOG_UNIT_NAME
#define VPUX_ELF_LOG_UNIT_NAME "VpuxHpi"
#endif
// clang-format off
#include <vpux_loader/vpux_loader.hpp>
#include <vpux_elf/utils/log.hpp>
#include <vpux_hpi.hpp>
#include <sstream>

#if defined(CONFIG_TARGET_SOC_3720) || defined(HOST_BUILD)
#include <hpi_3720.hpp>
#endif

#if defined(CONFIG_TARGET_SOC_4000) || defined(HOST_BUILD)
#include <hpi_4000.hpp>
#endif

#include <string.h>
// clang-format on

namespace elf {
namespace {

static std::unique_ptr<HostParsedInferenceCommon> getArchSpecificHPI(const elf::platform::ArchKind& archKind) {
    VPUX_ELF_LOG(LogLevel::LOG_DEBUG, "Creating specialized HPI for arch %u", archKind);

    std::unique_ptr<HostParsedInferenceCommon> archSpecificHPI;
    switch (archKind) {
#if defined(CONFIG_TARGET_SOC_3720) || defined(HOST_BUILD)
    case elf::platform::ArchKind::VPUX37XX:
        archSpecificHPI = std::make_unique<HostParsedInference_3720>();
        break;
#endif

#if defined(CONFIG_TARGET_SOC_4000) || defined(HOST_BUILD)
    case elf::platform::ArchKind::VPUX40XX:
        archSpecificHPI = std::make_unique<HostParsedInference_4000>();
        break;
#endif
    default:
        VPUX_ELF_THROW(RangeError, (elf::platform::stringifyArchKind(archKind) + " arch is not supported").c_str());
        break;
    }

    return archSpecificHPI;
}

}  // namespace

VersionsProvider::VersionsProvider(platform::ArchKind architecture) : impl(getArchSpecificHPI(architecture)) {}
VersionsProvider::~VersionsProvider() = default;
Version VersionsProvider::getLibraryELFVersion() const { return impl->getELFLibABIVersion(); }
Version VersionsProvider::getLibraryMIVersion() const { return impl->getStaticMIVersion(); }

const uint64_t* HostParsedInference::readPerfMetrics() {
    const auto& sections = loader->getSectionsOfType(elf::VPU_SHT_PERF_METRICS);
    VPUX_ELF_THROW_WHEN(sections.size() > 1, RangeError, "Expected only a single section of performance metrics.");

    if (sections.size() == 1) {
        return reinterpret_cast<const uint64_t*>(sections[0].cpu_addr());
    }

    VPUX_ELF_LOG(LogLevel::LOG_WARN, "No performance metrics. Default to be used!");
    return nullptr;
}

void HostParsedInference::readMetadata() {
    const auto& sections = loader->getSectionsOfType(elf::VPU_SHT_NETDESC);
    VPUX_ELF_THROW_UNLESS(sections.size() == 1, RangeError, "Expected only one metadata section.");

    auto metadataBufferPtr = sections[0].cpu_addr();
    auto metadataBufferSize = sections[0].size();
    metadata = MetadataSerialization::deserialize(metadataBufferPtr, metadataBufferSize);
}

void HostParsedInference::readPlatformInfo() {
    const auto& sections = loader->getSectionsOfType(elf::VPU_SHT_PLATFORM_INFO);
    VPUX_ELF_THROW_UNLESS(sections.size() == 1, RangeError, "Expected only one Platform Info section.");

    auto platformInfoBufferPtr = sections[0].cpu_addr();
    auto platformInfoBufferSize = sections[0].size();
    platformInfo = elf::platform::PlatformInfoSerialization::deserialize(platformInfoBufferPtr, platformInfoBufferSize);
}

elf::Version HostParsedInference::readVersioningInfo(uint32_t versionType) const {
    const auto& noteSections = loader->getSectionsOfType(elf::SHT_NOTE);
    for (auto section : noteSections) {
        VPUX_ELF_THROW_UNLESS(section.size() == sizeof(elf::elf_note::VersionNote), SectionError, "Wrong Versioning Note size");
        elf::elf_note::VersionNote elfABIVersionNote{};
        memcpy(&elfABIVersionNote, section.cpu_addr(), sizeof(elf::elf_note::VersionNote));
        if (elfABIVersionNote.n_type == versionType) {
            return elf::Version(elfABIVersionNote);
        }
    }
    VPUX_ELF_LOG(LogLevel::LOG_ERROR, "Could not retrieve versioning info of type %x", versionType);
    VPUX_ELF_THROW(RangeError, "Requested Versioning information was not found");
}

elf::Version HostParsedInference::getElfABIVersion() const {
    return readVersioningInfo(elf::elf_note::NT_GNU_ABI_TAG);
}

elf::Version HostParsedInference::getMIVersion() const {
    return readVersioningInfo(elf::elf_note::NT_NPU_MPI_VERSION);
}

elf::Version HostParsedInference::getLibraryELFVersion() const {
    return getArchSpecificHPI(hpiCfg.archKind)->getELFLibABIVersion();
}

elf::Version HostParsedInference::getLibraryMIVersion() const {
    return getArchSpecificHPI(hpiCfg.archKind)->getStaticMIVersion();
}

HostParsedInference::HostParsedInference(BufferManager* bufferMgr, AccessManager* accessMgr, elf::HPIConfigs hpiConfigs)
        : bufferManager(bufferMgr), accessManager(accessMgr), hpiCfg(hpiConfigs) {
    // create the loader object to cache sections
    loader = std::make_unique<VPUXLoader>(accessMgr, bufferMgr);

    auto& expectedArch = hpiConfigs.archKind;
    auto archSpecificHpi = getArchSpecificHPI(expectedArch);
    // Check ELF Library ABI Compatibility
    elf::Version::checkVersionCompatibility(getLibraryELFVersion(), getElfABIVersion(), elf::VersionType::ELF_ABI_VERSION);

    readMetadata();
    readPlatformInfo();

    auto archKind = platformInfo->mArchKind;

    // Check if compiled ELF arch and HPI arch match
    if (archKind != expectedArch) {
        std::stringstream logBuffer;
        logBuffer << "Incorrect arch. Expected: " << elf::platform::stringifyArchKind(expectedArch) << " vs Received: " << elf::platform::stringifyArchKind(archKind);
        VPUX_ELF_THROW(ArgsError, logBuffer.str().c_str());
    }

    // Check Mapped Inference Compatibility
    auto& nnExpectedVersion = hpiConfigs.nnVersion;

    // If Expected Mapped Inference version is not provided via HPI config, fall back to local version
    if (!nnExpectedVersion.checkValidity()) {
        nnExpectedVersion = getLibraryMIVersion();
    }

    elf::Version::checkVersionCompatibility(nnExpectedVersion, getMIVersion(), elf::VersionType::MAPPED_INFERENCE_VERSION);
}

void HostParsedInference::load() {
    auto archSpecificHpi = getArchSpecificHPI(platformInfo->mArchKind);

    const auto symbolTable = archSpecificHpi->getSymbolTable(metadata->mResourceRequirements.nn_slice_count_);
    const auto symbolSectionTypes = archSpecificHpi->getSymbolSectionTypes();
    auto symTabOverrideMode = archSpecificHpi->getSymbolSectionTypes().size() == 0 ? false : true;

    loader->load(symbolTable, symTabOverrideMode, symbolSectionTypes);

    parsedInference =
            std::make_shared<AllocatedDeviceBuffer>(bufferManager, archSpecificHpi->getParsedInferenceBufferSpecs());
    auto parsedInferenceBuffer = parsedInference->getBuffer();
    archSpecificHpi->setHostParsedInference(parsedInferenceBuffer, loader->getEntry(), metadata->mResourceRequirements,
                                            readPerfMetrics());
}

HostParsedInference::HostParsedInference(const HostParsedInference& other)
        : bufferManager(other.bufferManager), accessManager(other.accessManager), metadata(other.metadata), platformInfo(other.platformInfo) {
    auto archSpecificHpi = getArchSpecificHPI(platformInfo->mArchKind);
    // Use clone semantics here by copy-constructing the loader object
    loader = std::make_unique<VPUXLoader>(*other.loader);
    // Every new loader object means a new parsedInference struct as well
    parsedInference =
            std::make_shared<AllocatedDeviceBuffer>(bufferManager, archSpecificHpi->getParsedInferenceBufferSpecs());
    auto parsedInferenceBuffer = parsedInference->getBuffer();
    archSpecificHpi->setHostParsedInference(parsedInferenceBuffer, loader->getEntry(), metadata->mResourceRequirements,
                                            readPerfMetrics());
};

HostParsedInference::HostParsedInference(HostParsedInference&& other)
        : bufferManager(other.bufferManager),
          accessManager(other.accessManager),
          metadata(other.metadata),
          platformInfo(other.platformInfo),
          loader(std::move(other.loader)),
          parsedInference(other.parsedInference) {
}

HostParsedInference::~HostParsedInference() {
}

HostParsedInference& HostParsedInference::operator=(const HostParsedInference& rhs) {
    if (this == &rhs) {
        return *this;
    }

    bufferManager = rhs.bufferManager;
    accessManager = rhs.accessManager;
    metadata = rhs.metadata;
    platformInfo = rhs.platformInfo;

    auto archSpecificHpi = getArchSpecificHPI(platformInfo->mArchKind);
    // Use clone semantics here by copy-constructing the loader object
    loader = std::make_unique<VPUXLoader>(*rhs.loader);
    // Every new loader object means a new parsedInference struct as well
    parsedInference =
            std::make_shared<AllocatedDeviceBuffer>(bufferManager, archSpecificHpi->getParsedInferenceBufferSpecs());
    auto parsedInferenceBuffer = parsedInference->getBuffer();
    archSpecificHpi->setHostParsedInference(parsedInferenceBuffer, loader->getEntry(), metadata->mResourceRequirements,
                                            readPerfMetrics());

    return *this;
}

HostParsedInference& HostParsedInference::operator=(HostParsedInference&& rhs) {
    if (this == &rhs) {
        return *this;
    }
    bufferManager = rhs.bufferManager;
    accessManager = rhs.accessManager;
    metadata = rhs.metadata;
    platformInfo = rhs.platformInfo;
    loader = std::move(rhs.loader);
    parsedInference = rhs.parsedInference;

    return *this;
}

DeviceBuffer HostParsedInference::getParsedInference() const {
    return parsedInference->getBuffer();
}

std::vector<DeviceBuffer> HostParsedInference::getAllocatedBuffers() const {
    return loader->getAllocatedBuffers();
}

std::vector<DeviceBuffer> HostParsedInference::getInputBuffers() const {
    return loader->getInputBuffers();
}

std::vector<DeviceBuffer> HostParsedInference::getOutputBuffers() const {
    return loader->getOutputBuffers();
}

std::vector<DeviceBuffer> HostParsedInference::getProfBuffers() const {
    return loader->getProfBuffers();
}

std::shared_ptr<const elf::NetworkMetadata> HostParsedInference::getMetadata() {
    return metadata;
}

std::shared_ptr<const elf::platform::PlatformInfo> HostParsedInference::getPlatformInfo() {
    return platformInfo;
}

void HostParsedInference::applyInputOutput(std::vector<DeviceBuffer>& inputs, std::vector<DeviceBuffer>& outputs,
                                           std::vector<DeviceBuffer>& profiling) {
    return loader->applyJitRelocations(inputs, outputs, profiling);
}

}  // namespace elf
