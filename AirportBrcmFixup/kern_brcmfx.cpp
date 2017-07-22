//
//  kern_brcmfx.cpp
//  BRCMFX
//
//  Copyright © 2017 lvs1974. All rights reserved.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_file.hpp>

#include "kern_config.hpp"
#include "kern_brcmfx.hpp"

#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOReportTypes.h>
#include <IOKit/IOService.h>



// Only used in apple-driven callbacks
static BRCMFX *callbackBRCMFX {nullptr};
static KernelPatcher *callbackPatcher {nullptr};
static const IORegistryPlane* gIO80211FamilyPlane {nullptr};


static const char *idList[] {
    "com.apple.driver.AirPort.Brcm4360",
    "com.apple.driver.AirPort.BrcmNIC",
    "com.apple.driver.AirPort.BrcmNIC-MFG"
};

static const char *serviceNameList[] {
    "AirPort_Brcm4360",
    "AirPort_BrcmNIC",
    "AirPort_BrcmNIC_MFG"
};

static const int processingStateList[] {
    BRCMFX::ProcessingState::BRCM4360Patched,
    BRCMFX::ProcessingState::BRCMNICPatched,
    BRCMFX::ProcessingState::BRCMNIC_MFGPatched
};

static const char *binList[] {
    "/System/Library/Extensions/IO80211Family.kext/Contents/PlugIns/AirPortBrcm4360.kext/Contents/MacOS/AirPortBrcm4360",
    "/System/Library/Extensions/IO80211Family.kext/Contents/PlugIns/AirPortBrcmNIC.kext/Contents/MacOS/AirPortBrcmNIC",
    "/System/Library/Extensions/AirPortBrcmNIC-MFG.kext/Contents/MacOS/AirPortBrcmNIC-MFG"
};

static const char *symbolList[][4] {
    {"_si_pmu_fvco_pllreg",     "__ZNK16AirPort_Brcm436015newVendorStringEv",       "__ZN16AirPort_Brcm436012checkBoardIdEPKc",  "__ZN16AirPort_Brcm43605probeEP9IOServicePi"},
    {"_si_pmu_fvco_pllreg",     "__ZNK15AirPort_BrcmNIC15newVendorStringEv",        "__ZN15AirPort_BrcmNIC12checkBoardIdEPKc" , "__ZN15AirPort_BrcmNIC5probeEP9IOServicePi"},
    {"_si_pmu_fvco_pllreg",     "__ZNK19AirPort_BrcmNIC_MFG15newVendorStringEv",    "__ZN19AirPort_BrcmNIC_MFG12checkBoardIdEPKc", "__ZN19AirPort_BrcmNIC_MFG5probeEP9IOServicePi"}
};

static KernelPatcher::KextInfo kextList[] {
    { idList[0], &binList[0], 1, true, {}, KernelPatcher::KextInfo::Unloaded },
    { idList[1], &binList[1],  1, true, {}, KernelPatcher::KextInfo::Unloaded },
    { idList[2], &binList[2],  1, true, {}, KernelPatcher::KextInfo::Unloaded }
};

static const size_t kextListSize {3};


//==============================================================================

bool BRCMFX::init()
{
    gIO80211FamilyPlane	= IORegistryEntry::makePlane( "IO80211Plane" );
    
    LiluAPI::Error error = lilu.onKextLoad(kextList, kextListSize,
                                           [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
                                               callbackBRCMFX = static_cast<BRCMFX *>(user);
                                               callbackPatcher = &patcher;
                                               callbackBRCMFX->processKext(patcher, index, address, size);
                                           }, this);
    
    if (error != LiluAPI::Error::NoError) {
        SYSLOG("BRCMFX @ failed to register onKextLoad method %d", error);
        return false;
    }
    
	return true;
}

//==============================================================================

void BRCMFX::deinit()
{
}

//==============================================================================

bool BRCMFX::checkBoardId(const char *boardID)
{
    if (callbackBRCMFX && callbackPatcher && callbackBRCMFX->orgCheckBoardId)
        return true;
    return false;
}

//==============================================================================

const OSSymbol* BRCMFX::newVendorString(void)
{
    return OSSymbol::withCString("Apple");
}

//==============================================================================

IOService* BRCMFX::probe(IOService* provider, SInt32* score )
{
    return nullptr;
}

//==============================================================================

IOService* BRCMFX::findService(const IORegistryPlane* plane, const char *service_name)
{
    IOService            * service = 0;
    IORegistryIterator   * iter = IORegistryIterator::iterateOver(plane, kIORegistryIterateRecursively);
    OSOrderedSet         * all = 0;
    
    if ( iter)
    {
        do
        {
            if (all)
                all->release();
            all = iter->iterateAll();
        }
        while (!iter->isValid());
        iter->release();
        
        if (all)
        {
            while ((service = OSDynamicCast(IOService, all->getFirstObject())))
            {
                all->removeObject(service);
                if (strcmp(service->getName(), service_name) == 0)
                    break;
            }
            
            all->release();
        }
    }
    
    return service;
}

//==============================================================================

bool BRCMFX::startService(IOService* service)
{
    bool result = false;
    IOService *provider = service->getProvider();
    if (provider != nullptr)
    {
        OSString* bundle = OSDynamicCast(OSString, provider->getProperty("CFBundleIdentifier"));
        DBGLOG("BRCMFX @ provider name = %s, bundle = %s", provider->getName(), (bundle != nullptr) ? bundle->getCStringNoCopy() : "");
        
        SInt32 score = 0;
        DBGLOG("BRCMFX @ probe is called for %s with provider %s", service->getName(), provider->getName());
        if (service->attach(provider))
        {
            IOService *service_to_start = service->probe(provider, &score);
            service->detach(provider);
            IOSleep(300);
            if (service_to_start != nullptr)
            {
                service_to_start->retain();
                if (service_to_start->attach(provider))
                {
                    result = service_to_start->start(provider);
                    if (!result)
                    {
                        SYSLOG("BRCMFX @ service %s can't be started", service_to_start->getName());
                        service->detach( provider );
                    }
                }
                else
                    SYSLOG("BRCMFX @ service %s can't be attached to provider", service_to_start->getName());
                service_to_start->release();
            }
            else
                SYSLOG("BRCMFX @ service %s probe returned null", service->getName());
        }
    }
    else
        SYSLOG("BRCMFX @ service %s doesn't have provider", service->getName());
    return result;
}

/*******************************************************************************
// This assembler function is needed since method _si_pmu_fvco_pllreg uses user calling convention (rdi, rsi, rdx, rax)
// (more details on https://en.wikipedia.org/wiki/X86_calling_conventions#System_V_AMD64_ABI)
// _si_pmu_fvco_pllreg (10.11 - 10.13) compares value stored at [rdi+0x3c] with 0xaa52. This value is a chip identificator: 43602, details:
// https://chromium.googlesource.com/chromiumos/third_party/kernel/+/chromeos-3.18/drivers/net/wireless/bcmdhd/include/bcmdevs.h#396
// In 10.12 and earlier _si_pmu_fvco_pllreg will fail if the value stored at [rdi+0x3c] is not 0xaa52, driver won't be loaded
// The idea behind of this assembler function (credits to vit9696):
// - save rdi on stack 
// - save current value at [rdi+0x3c] on stack (it may be something different from 0xaa52, we will restore it later)
// - set a field of data structure to the expected value 0xaa52
// - call the next instruction after call (by address 11) -> it's a common way to retrieve an absolute execution pointer (rip) (0x11 in this example)
// - add 8 bytes to the rip (skip opcodes 59 48 83 C1 08 51 EB 03) in order to calculate an address of the instruction "pop rcx" (0x19)
// - save the calculated value on stack (it will be used as a return address by instruction ret in _si_pmu_fvco_pllreg)
// - jump to block of opcodes, generated by Lilu (to address 1F), finally the original method _si_pmu_fvco_pllreg will be called
// _si_pmu_fvco_pllreg is executed and afterwards returns to address 19.
// - restore rcx and rdi
// - set the stored value at [rdi+0x3c]
// - return to the caller (_wlc_proxd_attach or _pdburst_collection)
 
 Assembler wrapper for calling _si_pmu_fvco_pllreg:
 00000000000000 57                     push       rdi                       // entry point, when _wlc_proxd_attach calls _si_pmu_fvco_pllreg, we are here
 00000000000001 8B4F3C                 mov        ecx, dword [rdi+0x3c]
 00000000000004 51                     push       rcx
 00000000000005 C7473C52AA0000         mov        dword [rdi+0x3c], 0xaa52
 0000000000000C E800000000             call       $+5                       // calls the next instruction to retrieve rip
 00000000000011 59                     pop        rcx                       // rcx = 0x11 (absolute address in a real life)
 00000000000012 4883C108               add        rcx, 0x8                  // add 0x8 to get an absolute address of instruction "pop rcx"
 00000000000016 51                     push       rcx                       // push an absolute address (will be used by instruction ret later on)
 00000000000017 EB06                   jmp        $+6                       // jump to Lilu_wrapper, _si_pmu_fvco_pllreg will be called
 00000000000019 59                     pop        rcx                       // we are here after return from _si_pmu_fvco_pllreg performs
 0000000000001A 5F                     pop        rdi
 0000000000001B 894F3C                 mov        dword [rdi+0x3c], ecx     // restore saved value
 0000000000001E C3                     ret                                  // return to _wlc_proxd_attach
 ******************************************************************************
 These opcodes were written and generated by Lilu automatically (by routeBlock)
 Lilu_wrapper:
 0000000000001F 55 48 89 e5 41 57 41 56 53 50 49 89 d6 48 89 f3             // this is the copy of _si_pmu_fvco_pllreg (the beginning)
 0000000000002E e9 20 95 cb fe                                              // jump to the rest of original part _si_pmu_fvco_pllreg's body
 */

static const uint8_t opcodes[] = {
    0x57, 0x8B, 0x4F, 0x3C, 0x51, 0xC7,
    0x47, 0x3C, 0x52, 0xAA, 0x00, 0x00,
    0xE8, 0x00, 0x00, 0x00, 0x00, 0x59,
    0x48, 0x83, 0xC1, 0x08, 0x51, 0xEB,
    0x06, 0x59, 0x5F, 0x89, 0x4F, 0x3C,
    0xC3
};

//==============================================================================

void BRCMFX::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
    if (progressState != ProcessingState::EverythingDone)
    {
        for (size_t i = 0; i < kextListSize; i++)
        {
            if (kextList[i].loadIndex == index)
            {
                while (!(progressState & processingStateList[i]))
                {
                    progressState |= processingStateList[i];
                    
                    DBGLOG("BRCMFX @ found %s", idList[i]);

                    const char *method_name = symbolList[i][0];
                    auto method_address = patcher.solveSymbol(index, method_name);
                    if (method_address) {
                        DBGLOG("BRCMFX @ obtained %s", method_name);
                        patcher.routeBlock(method_address, opcodes, sizeof(opcodes), true);
                        if (patcher.getError() == KernelPatcher::Error::NoError) {
                            DBGLOG("BRCMFX @ routed %s", symbolList[i][0]);
                        } else {
                            SYSLOG("BRCMFX @ failed to route %s", method_name);
                            break;
                        }
                    } else {
                        SYSLOG("BRCMFX @ failed to resolve %s", method_name);
                        break;
                    }
                    
                    method_name = symbolList[i][1];
                    method_address = patcher.solveSymbol(index, method_name);
                    if (method_address) {
                        DBGLOG("BRCMFX @ obtained %s", method_name);
                        orgNewVendorString = reinterpret_cast<t_new_vendorstring>(patcher.routeFunction(method_address, reinterpret_cast<mach_vm_address_t>(newVendorString), true));
                        if (patcher.getError() == KernelPatcher::Error::NoError) {
                            DBGLOG("BRCMFX @ routed %s", method_name);
                        } else {
                            SYSLOG("BRCMFX @ failed to route %s", method_name);
                            break;
                        }
                    } else {
                        SYSLOG("BRCMFX @ failed to resolve %s", method_name);
                        break;
                    }
                    
                    method_name = symbolList[i][2];
                    method_address = patcher.solveSymbol(index, method_name);
                    if (method_address) {
                        DBGLOG("BRCMFX @ obtained %s", method_name);
                        orgCheckBoardId = reinterpret_cast<t_check_board_Id>(patcher.routeFunction(method_address, reinterpret_cast<mach_vm_address_t>(checkBoardId), true));
                        if (patcher.getError() == KernelPatcher::Error::NoError) {
                            DBGLOG("BRCMFX @ routed %s", method_name);
                        } else {
                            SYSLOG("BRCMFX @ failed to route %s", method_name);
                            break;
                        }
                    } else {
                        SYSLOG("BRCMFX @ failed to resolve %s", method_name);
                        break;
                    }

                    // IO80211FamilyPlane should keep a pointer to broadcom driver (even if it's not started/probed)
                    IOService* service = findService(gIO80211FamilyPlane, serviceNameList[i]);
                    // IOServicePlane should keep a pointer to broadcom driver only if it was successfully started
                    IOService* running_service = findService(gIOServicePlane, serviceNameList[i]);
                    if (service != nullptr)
                        DBGLOG("BRCMFX @ %s IO80211FamlilyPlane: service state: %08x", serviceNameList[i], service->getState());
                    if (running_service != nullptr)
                        DBGLOG("BRCMFX @ %s IOServicePlane: service state: %08x", serviceNameList[i], running_service->getState());

                    // In 10.12 (and probably earlier) BRCMFX::processKext is called too late, after failed attempt to start broadcom driver,
                    // so we have to try to start it again after all required patches
                    if (service != nullptr && running_service == nullptr && startService(service))
                    {
                        SYSLOG("BRCMFX @ service %s successfully started", service->getName());
                        
                        // we have to disable probing in the future, otherwise system can try to run this service again (10.13)
                        method_name = symbolList[i][3];
                        method_address = patcher.solveSymbol(index, method_name);
                        if (method_address) {
                            DBGLOG("BRCMFX @ obtained %s", method_name);
                            patcher.routeFunction(method_address, reinterpret_cast<mach_vm_address_t>(probe), true);
                            if (patcher.getError() == KernelPatcher::Error::NoError) {
                                DBGLOG("BRCMFX @ routed %s", method_name);
                            } else {
                                SYSLOG("BRCMFX @ failed to route %s", method_name);
                            }
                        } else {
                            SYSLOG("BRCMFX @ failed to resolve %s", method_name);
                        }
                    }
                    break;
                }
            }
        }
    }
    
    // Ignore all the errors for other processors
    patcher.clearError();
}

