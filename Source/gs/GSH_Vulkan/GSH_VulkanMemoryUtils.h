#pragma once

#include <type_traits>
#include "nuanceur/Builder.h"
#include "../GsPixelFormats.h"

namespace GSH_Vulkan
{
	class CMemoryUtils
	{
	public:
		template <typename StorageFormat>
		static Nuanceur::CIntRvalue GetPixelAddress(Nuanceur::CShaderBuilder& b, Nuanceur::CImageUint2DValue swizzleTable,
		                                            Nuanceur::CIntValue bufAddress, Nuanceur::CIntValue bufWidth, Nuanceur::CInt2Value position)
		{
			auto ux = ToUint(position->x());
			auto uy = ToUint(position->y());
			auto ubw = ToUint(bufWidth);
			auto uaddr = ToUint(bufAddress);

			if constexpr(std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMCT32> ||
			             std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMZ32>)
			{
				auto pageNum = (ux >> NewUint(b, 6)) + (((uy >> NewUint(b, 5)) * ubw) >> NewUint(b, 6));
				auto pageOffset =
				    ((ux & NewUint(b, 1)) << NewUint(b, 2)) |
				    ((uy & NewUint(b, 1)) << NewUint(b, 3)) |
				    ((ux & NewUint(b, 6)) << NewUint(b, 3)) |
				    ((uy & NewUint(b, 6)) << NewUint(b, 5)) |
				    ((ux & NewUint(b, 8)) << NewUint(b, 5)) |
				    ((uy & NewUint(b, 8)) << NewUint(b, 6)) |
				    ((ux & NewUint(b, 16)) << NewUint(b, 6)) |
				    ((uy & NewUint(b, 16)) << NewUint(b, 7)) |
				    ((ux & NewUint(b, 32)) << NewUint(b, 7));
				if constexpr(std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMZ32>)
				{
					return ToInt(uaddr + (pageNum << NewUint(b, 13)) + (pageOffset ^ NewUint(b, 0x1800)));
				}
				else
				{
					return ToInt(uaddr + (pageNum << NewUint(b, 13)) + pageOffset);
				}
			}
			else if constexpr(std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMCT16> ||
			                  std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMZ16>)
			{
				auto pageNum = (ux >> NewUint(b, 6)) + (((uy >> NewUint(b, 6)) * ubw) >> NewUint(b, 6));
				auto pageOffset =
				    ((ux & NewUint(b, 8)) >> NewUint(b, 2)) |
				    ((ux & NewUint(b, 1)) << NewUint(b, 2)) |
				    ((uy & NewUint(b, 1)) << NewUint(b, 3)) |
				    ((ux & NewUint(b, 6)) << NewUint(b, 3)) |
				    ((uy & NewUint(b, 14)) << NewUint(b, 5)) |
				    ((ux & NewUint(b, 16)) << NewUint(b, 5)) |
				    ((uy & NewUint(b, 16)) << NewUint(b, 6)) |
				    ((ux & NewUint(b, 32)) << NewUint(b, 6)) |
				    ((uy & NewUint(b, 32)) << NewUint(b, 7));
				if constexpr(std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMZ16>)
				{
					return ToInt(uaddr + (pageNum << NewUint(b, 13)) + (pageOffset ^ NewUint(b, 0x1800)));
				}
				else
				{
					return ToInt(uaddr + (pageNum << NewUint(b, 13)) + pageOffset);
				}
			}
			else if constexpr(std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMCT16S> ||
			                  std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMZ16S>)
			{
				auto pageNum = (ux >> NewUint(b, 6)) + (((uy >> NewUint(b, 6)) * ubw) >> NewUint(b, 6));
				auto pageOffset =
				    ((ux & NewUint(b, 8)) >> NewUint(b, 2)) |
				    ((ux & NewUint(b, 1)) << NewUint(b, 2)) |
				    ((uy & NewUint(b, 1)) << NewUint(b, 3)) |
				    ((ux & NewUint(b, 6)) << NewUint(b, 3)) |
				    ((uy & NewUint(b, 14)) << NewUint(b, 5)) |
				    ((ux & NewUint(b, 16)) << NewUint(b, 5)) |
				    ((uy & NewUint(b, 32)) << NewUint(b, 5)) |
				    ((uy & NewUint(b, 16)) << NewUint(b, 7)) |
				    ((ux & NewUint(b, 32)) << NewUint(b, 7));
				if constexpr(std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMZ16S>)
				{
					return ToInt(uaddr + (pageNum << NewUint(b, 13)) + (pageOffset ^ NewUint(b, 0x1800)));
				}
				else
				{
					return ToInt(uaddr + (pageNum << NewUint(b, 13)) + pageOffset);
				}
			}
			else if constexpr(std::is_same_v<StorageFormat, CGsPixelFormats::STORAGEPSMT8>)
			{
				auto pageNum = (ux >> NewUint(b, 7)) + (((uy >> NewUint(b, 6)) * ubw) >> NewUint(b, 7));
				auto swapBit = (((ux >> NewUint(b, 2)) ^ (uy >> NewUint(b, 1)) ^ (uy >> NewUint(b, 2))) & NewUint(b, 1)) << NewUint(b, 5);
				auto pageOffset =
				    ((ux & NewUint(b, 8)) >> NewUint(b, 2)) |
				    ((uy & NewUint(b, 2)) >> NewUint(b, 1)) |
				    ((ux & NewUint(b, 1)) << NewUint(b, 2)) |
				    ((uy & NewUint(b, 1)) << NewUint(b, 3)) |
				    ((ux & NewUint(b, 2)) << NewUint(b, 3)) |
				    swapBit |
				    ((uy & NewUint(b, 12)) << NewUint(b, 4)) |
				    ((ux & NewUint(b, 16)) << NewUint(b, 4)) |
				    ((uy & NewUint(b, 16)) << NewUint(b, 5)) |
				    ((ux & NewUint(b, 32)) << NewUint(b, 5)) |
				    ((uy & NewUint(b, 32)) << NewUint(b, 6)) |
				    ((ux & NewUint(b, 64)) << NewUint(b, 6));
				return ToInt(uaddr + (pageNum << NewUint(b, 13)) + pageOffset);
			}
			else
			{
				auto pageWidth = NewInt(b, StorageFormat::PAGEWIDTH);
				auto pageHeight = NewInt(b, StorageFormat::PAGEHEIGHT);
				auto pageSize = NewInt(b, CGsPixelFormats::PAGESIZE);
				auto pageNum = (position->x() / pageWidth) + ((position->y() / pageHeight) * bufWidth) / pageWidth;
				auto pagePos = NewInt2(position->x() % pageWidth, position->y() % pageHeight);
				auto pageOffset = ToInt(Load(swizzleTable, pagePos)->x());
				return bufAddress + (pageNum * pageSize) + pageOffset;
			}
		}

		static Nuanceur::CIntRvalue GetPixelAddress_PSMT4(Nuanceur::CShaderBuilder&, Nuanceur::CImageUint2DValue, Nuanceur::CIntValue, Nuanceur::CIntValue, Nuanceur::CInt2Value);

		static Nuanceur::CUintRvalue Memory_Read32(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue);
		static Nuanceur::CUintRvalue Memory_Read24(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue);
		static Nuanceur::CUintRvalue Memory_Read16(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue);
		static Nuanceur::CUintRvalue Memory_Read8(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue);
		static Nuanceur::CUintRvalue Memory_Read4(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue);

		static void Memory_Write32(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue, Nuanceur::CUintValue);
		static void Memory_Write24(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue, Nuanceur::CUintValue);
		static void Memory_Write24(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUcharValue, Nuanceur::CArrayUshortValue, Nuanceur::CIntValue, Nuanceur::CUintValue);
		static void Memory_Write16(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue, Nuanceur::CUintValue);
		static void Memory_Write16(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUshortValue, Nuanceur::CIntValue, Nuanceur::CUintValue);
		static void Memory_Write8(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue, Nuanceur::CUintValue);
		static void Memory_Write8(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUcharValue, Nuanceur::CIntValue, Nuanceur::CUintValue);
		static void Memory_Write4(Nuanceur::CShaderBuilder&, Nuanceur::CArrayUintValue, Nuanceur::CIntValue, Nuanceur::CUintValue);

		static Nuanceur::CFloat4Rvalue PSM32ToVec4(Nuanceur::CShaderBuilder&, Nuanceur::CUintValue);
		static Nuanceur::CFloat4Rvalue PSM16ToVec4(Nuanceur::CShaderBuilder&, Nuanceur::CUintValue);

		static Nuanceur::CUintRvalue Vec4ToPSM32(Nuanceur::CShaderBuilder&, Nuanceur::CFloat4Value);
		static Nuanceur::CUintRvalue Vec4ToPSM16(Nuanceur::CShaderBuilder&, Nuanceur::CFloat4Value);

		static Nuanceur::CInt4Rvalue PSM32ToIVec4(Nuanceur::CShaderBuilder&, Nuanceur::CUintValue);
		static Nuanceur::CInt4Rvalue PSM16ToIVec4(Nuanceur::CShaderBuilder&, Nuanceur::CUintValue);

		static Nuanceur::CUintRvalue IVec4ToPSM32(Nuanceur::CShaderBuilder&, Nuanceur::CInt4Value);
		static Nuanceur::CUintRvalue IVec4ToPSM16(Nuanceur::CShaderBuilder&, Nuanceur::CInt4Value);
	};
}
