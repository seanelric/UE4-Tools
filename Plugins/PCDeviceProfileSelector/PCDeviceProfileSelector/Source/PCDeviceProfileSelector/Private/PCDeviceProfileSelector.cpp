// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCDeviceProfileSelectorModule.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "Misc/ConfigCacheIni.h"
#include <regex>
#include <string>

IMPLEMENT_MODULE(FPCDeviceProfileSelectorModule, PCDeviceProfileSelector)

namespace
{
    FString FindMatchingProfile(const FString& DeviceChipName, const TCHAR* DeviceMarkSection, const TCHAR* DeviceQualitySection,
        int32& DeviceScore, int32& OutQualityLevel)
    {
        FString WindowsDevicsProfileName;

        // Find current device score.
        TArray<FString> ProfileList;
        if (GConfig->GetInt(DeviceMarkSection, *DeviceChipName, DeviceScore, GDeviceProfilesIni)
            && GConfig->GetSection(DeviceQualitySection, ProfileList, GDeviceProfilesIni))
        {
            UE_LOG(LogWindows, Log, TEXT("* Device score: [%i]"), DeviceScore);

            // Profile matching.
            for (int32 i = 0; i < ProfileList.Num(); ++i)
            {
                FString ProfileName, QualityDeviceName;
                if (!ProfileList[i].Split(TEXT("="), &ProfileName, &QualityDeviceName))
                    continue;

                // Compare score with each quality level.
                int32 QualityScore = 0;
                if (!GConfig->GetInt(DeviceMarkSection, *QualityDeviceName, QualityScore, GDeviceProfilesIni))
                    continue;

                if (i > 0 && DeviceScore < QualityScore)
                    continue;

                WindowsDevicsProfileName = ProfileName;
                OutQualityLevel = i;
            }
        }

        //UE_LOG(LogWindows, Log, TEXT("Matched %s as %s[%i]"), *Name, *WindowsDevicsProfileName, OutQualityLevel);

        return WindowsDevicsProfileName;
    }

    FString CleanDeviceNameWithPattern(const FString& Original, const std::regex& Pattern)
    {
        // 1. 转换小写并清除商标
        FString LowerName = Original.ToLower();
        
        LowerName.ReplaceInline(TEXT("\u00AE"), TEXT(" ")); // ®
        LowerName.ReplaceInline(TEXT("\u2122"), TEXT(" ")); // ™
        LowerName.ReplaceInline(TEXT("\u00A9"), TEXT(" ")); // ©

        std::string CleanedName(TCHAR_TO_UTF8(*LowerName));

        // 2. 按规则清洗型号
        CleanedName = std::regex_replace(CleanedName, Pattern, " ");

        // 3. 清理多余空格
        static const std::regex WhitespacePattern(R"(\s+)");
        CleanedName = std::regex_replace(CleanedName, WhitespacePattern, " ");

        FString Result(UTF8_TO_TCHAR(CleanedName.c_str()));
        return Result.TrimStartAndEnd();
    }

    FString CleanCPUName(const FString& Original)
    {
        // 首分支匹配带 [...] 前缀的整个型号，清洗为空后按未匹配设备处理。
        static const std::regex CPUPattern(
            R"(^\s*\[[^\]]*\].*$|\((?:r|tm|c)\)|®|™|©|\b\d+(?:st|nd|rd|th)\s+gen(?:eration)?\b|\s*@\s*\d+(?:\.\d+)?\s*[gm]hz\b|\s+(?:with|w/)\s+radeon\b.*$|\b\w+-core\s+processor\b|\b(?:cpu|processor|apu)\b)"
        );

        return CleanDeviceNameWithPattern(Original, CPUPattern);
    }

    FString CleanGPUName(const FString& Original)
    {
        static const std::regex GPUPattern(
            R"(\((?:r|tm|c)\)|^\s*(?:(?:nvidia|amd|ati)\b\s*)+|\b(?:gpu|adapter|series)\b|[()])"
        );

        return CleanDeviceNameWithPattern(Original, GPUPattern);
    }
    
    void GetDeviceDisplayInfo(FDisplayMetrics& DisplayMetrics, FMonitorInfo& OutMonitorInfo)
    {
        // Retrieve display metrics.
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().GetInitialDisplayMetrics(DisplayMetrics);
        }
        else
        {
            if (FApp::CanEverRender())
            {
                FDisplayMetrics::RebuildDisplayMetrics(DisplayMetrics);
            }
            else
            {
                // assume headless
                DisplayMetrics.PrimaryDisplayWidth = 1920;
                DisplayMetrics.PrimaryDisplayHeight = 1080;
            }
        }

        // Match current used monitor.
        if (DisplayMetrics.MonitorInfo.Num() > 0)
        {
            OutMonitorInfo = DisplayMetrics.MonitorInfo[0];
            for (int32 Index = 1; Index < DisplayMetrics.MonitorInfo.Num(); ++Index)
            {
                const FMonitorInfo& Info = DisplayMetrics.MonitorInfo[Index];
                if (Info.bIsPrimary)
                {
                    OutMonitorInfo = Info;
                    break;
                }
            }
        }
    }

    FIntPoint GetDeviceMaxSize()
    {
        FIntPoint DisplayResolution;

        FDisplayMetrics DisplayMetrics;
        FMonitorInfo MonitorInfo;
        GetDeviceDisplayInfo(DisplayMetrics, MonitorInfo);

        if (MonitorInfo.Name.IsEmpty() || MonitorInfo.NativeWidth <= 0 || MonitorInfo.NativeHeight <= 0) {
            DisplayResolution = FIntPoint(DisplayMetrics.PrimaryDisplayWidth, DisplayMetrics.PrimaryDisplayHeight);
        } else {
            DisplayResolution = FIntPoint(MonitorInfo.NativeWidth, MonitorInfo.NativeHeight);
        }

        return DisplayResolution;
    }

    FIntPoint MatchRecommendResolution(const int32 DeviceScore)
    {
        FIntPoint Result = GetDeviceMaxSize();

        // Only handle 16:9 device(for simple), other use native resolution.
        TArray<FString> ResolutionGradeList;
        if (FMath::IsNearlyEqual(static_cast<float>(Result.X) / Result.Y, 16.0f / 9)
            && GConfig->GetSection(TEXT("ResolutionQualityGrade"), ResolutionGradeList, GDeviceProfilesIni))
        {
            for (int32 i = ResolutionGradeList.Num() - 1; i >= 0; --i)
            {
                FString ResStr, QualityScore;
                if (!ResolutionGradeList[i].Split(TEXT("="), &ResStr, &QualityScore))
                    continue;

                if (DeviceScore >= FCString::Atoi(*QualityScore))
                {
                    FString Width, Height;
                    if (!ResStr.Split(TEXT("x"), &Width, &Height))
                        continue;

                    Result.X = FMath::Min(FCString::Atoi(*Width), Result.X);
                    Result.Y = FMath::Min(FCString::Atoi(*Height), Result.Y);
                    break;
                }
            }
        }

        return Result;
    }
}

void FPCDeviceProfileSelectorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FPCDeviceProfileSelectorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

const FString FPCDeviceProfileSelectorModule::GetRuntimeDeviceProfileName() {
    FString WindowsDevicsProfileName;

    // Custom graphics quality classification.
    if (FApp::CanEverRender()) {
        UE_LOG(LogWindows, Log, TEXT("*************************************************************************************"));

        // CPU
        int32 OutCPUScore = 0;
        int32 OutCPUQualityLevel = 0;

        const FString CPUBrand = FPlatformMisc::GetCPUBrand();
        UE_LOG(LogWindows, Log, TEXT("* CPU: [%s]"), *CPUBrand);
        
        const FString CleanedCPUBrand = CleanCPUName(CPUBrand);
        UE_LOG(LogWindows, Log, TEXT("* CPU cleaned: [%s]"), *CleanedCPUBrand);
        
        FString CPUProfileName = FindMatchingProfile(CleanedCPUBrand, TEXT("DeviceCPUMarks"), TEXT("DeviceQualityCPUGrade"), OutCPUScore, OutCPUQualityLevel);
        if (!CPUProfileName.IsEmpty())
        {
            UE_LOG(LogWindows, Log, TEXT("* CPU matched quality[0~4]: [%i]"), OutCPUQualityLevel);   
        }
        UE_LOG(LogWindows, Log, TEXT("*"));
        
        // GPU
        int32 OutGPUScore = 0;
        int32 OutGPUQualityLevel = 0;

        FString GPUBrand = FPlatformMisc::GetPrimaryGPUBrand();
        if (!GRHIAdapterName.IsEmpty() && !GRHIAdapterName.Equals(GPUBrand)) {
            GPUBrand = GRHIAdapterName;
        }
        UE_LOG(LogWindows, Log, TEXT("* GPU: [%s]"), *GPUBrand);

        const FString CleanedGPUBrand = CleanGPUName(GPUBrand);
        UE_LOG(LogWindows, Log, TEXT("* GPU cleaned: [%s]"), *CleanedGPUBrand);

        FString GPUProfileName = FindMatchingProfile(CleanedGPUBrand, TEXT("DeviceGPUMarks"), TEXT("DeviceQualityGPUGrade"), OutGPUScore, OutGPUQualityLevel);
        if (!GPUProfileName.IsEmpty())
        {
            UE_LOG(LogWindows, Log, TEXT("* GPU matched quality[0~4]: [%i]"), OutGPUQualityLevel);   
        }
        UE_LOG(LogWindows, Log, TEXT("*"));

        // Select lower quality profile.
        if (!(CPUProfileName.IsEmpty() && GPUProfileName.IsEmpty()))
        {
            // If one of them fails to match, select between matched and high quality.
            if (CPUProfileName.IsEmpty())
            {
                CPUProfileName = "Windows_Mid";
                OutCPUQualityLevel = 2;
            }
            if (GPUProfileName.IsEmpty())
            {
                GPUProfileName = "Windows_Mid";
                OutGPUQualityLevel = 2;
            }
            WindowsDevicsProfileName = (OutCPUQualityLevel <= OutGPUQualityLevel) ? CPUProfileName : GPUProfileName;   
        }

        // Resolution recommend.
        FIntPoint MaxSize = GetDeviceMaxSize();
        UE_LOG(LogWindows, Log, TEXT("* Device max resolution: [%ix%i]"), MaxSize.X, MaxSize.Y);
        UE_LOG(LogWindows, Log, TEXT("*"));

        FIntPoint RecommendRes = MatchRecommendResolution(OutGPUScore);
        UE_LOG(LogWindows, Log, TEXT("* Recommend resolution: [%ix%i]"), RecommendRes.X, RecommendRes.Y);
        UE_LOG(LogWindows, Log, TEXT("*"));
    }

    if (WindowsDevicsProfileName.IsEmpty()) {
        // Windows, WindowsNoEditor, WindowsClient, or WindowsServer
        WindowsDevicsProfileName = FPlatformProperties::PlatformName();
    }
    UE_LOG(LogWindows, Log, TEXT("* Final selected Device Profile: [%s]"), *WindowsDevicsProfileName);
    UE_LOG(LogWindows, Log, TEXT("*************************************************************************************"));

    return WindowsDevicsProfileName;
}
