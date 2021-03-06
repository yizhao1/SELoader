/*
 * Copyright (c) 2017, Wind River Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1) Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2) Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3) Neither the name of Wind River Systems nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author:
 *       Jia Zhang <zhang.jia@linux.alibaba.com>
 */

#include <Efi.h>
#include <EfiLibrary.h>
#include <BaseLibrary.h>

#if GNU_EFI_VERSION <= 303
EFI_GUID gEfiSimpleFileSystemProtocolGuid =
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
#endif

STATIC EFI_STATUS
OpenRootDirectory(EFI_FILE_HANDLE *RootDirectoryHandle)
{
	EFI_FILE_IO_INTERFACE *FileSystem;
	EFI_STATUS Status;

	Status = EfiProtocolOpen(gThisDevice,
				 &gEfiSimpleFileSystemProtocolGuid,
				 (VOID **)&FileSystem);
	if (EFI_ERROR(Status))
		return Status;

	Status = FileSystem->OpenVolume(FileSystem, RootDirectoryHandle);
	if (EFI_ERROR(Status))
		EfiConsolePrintError(L"Failed to open volume (err: 0x%x)\n",
				     Status);

	return Status;
}

STATIC EFI_STATUS
OpenFile(CONST CHAR16 *Path, UINT64 OpenMode, EFI_FILE_HANDLE *FileHandle,
	 EFI_FILE_HANDLE *RootDirectoryHandle)
{
	EFI_FILE_HANDLE Root;
	EFI_STATUS Status;

	Status = OpenRootDirectory(&Root);
	if (EFI_ERROR(Status))
		return Status;

	CHAR16 *FilePath;

	Status = EfiDevicePathCreate(Path, &FilePath);
	if (EFI_ERROR(Status)) {
		Root->Close(Root);
		return Status;
	}

	Status = Root->Open(Root, FileHandle, FilePath, OpenMode, 0);

	if (!RootDirectoryHandle)
		Root->Close(Root);

	if (EFI_ERROR(Status)) {
		if (Status != EFI_NOT_FOUND)
			EfiConsolePrintError(L"Failed to open file %s for %s "
					     L"(err: 0x%x)\n", FilePath,
					     OpenMode & EFI_FILE_MODE_WRITE ?
					     L"write" : L"read", Status);
		EfiMemoryFree(FilePath);
		return Status;
	}
	EfiMemoryFree(FilePath);

	if (RootDirectoryHandle)
		*RootDirectoryHandle = Root;

	return Status;
}

STATIC EFI_STATUS
LoadFile(CONST CHAR16 *Path, CONST CHAR16 *Suffix, VOID **Data,
	 UINTN *DataSize)
{
	CHAR16 *FilePath;

	if (!Suffix)
		FilePath = (CHAR16 *)Path;
	else {
		FilePath = StrAppend(Path, Suffix);
		if (!FilePath)
			return EFI_OUT_OF_RESOURCES;
	}

	EFI_FILE_HANDLE FileHandle;
	EFI_STATUS Status;

	Status = OpenFile(FilePath, EFI_FILE_MODE_READ, &FileHandle, NULL);
	if (EFI_ERROR(Status))
		goto ErrOnOpenFile;

	/* Open testing */
	if (!Data && !DataSize)
		goto ErrOnOpenFile;

	EFI_FILE_INFO *FileInfo;

	FileInfo = LibFileInfo(FileHandle);
	if (!FileInfo) {
		EfiConsolePrintError(L"Failed to get file info for %s\n",
				     FilePath);
		Status = EFI_OUT_OF_RESOURCES;
		goto ErrOnReadFileInfo;
	}

	UINTN FileSize = (UINTN)FileInfo->FileSize;

	EfiMemoryFree(FileInfo);

	/* Request file size */
	if (!Data && DataSize) {
		*DataSize = FileSize;
		goto ErrOnAllocFileBuffer;
	}

	VOID *FileBuffer = NULL;

	if (FileSize) {
		if (!*Data) {
			/* Return truncated length */
			if (*DataSize && *DataSize < FileSize)
				FileSize = *DataSize;

			Status = EfiMemoryAllocate(FileSize, &FileBuffer);
			if (EFI_ERROR(Status))
				goto ErrOnAllocFileBuffer;
		} else
			FileBuffer = *Data;

		Status = FileHandle->Read(FileHandle, &FileSize, FileBuffer);
		if (EFI_ERROR(Status)) {
			EfiConsolePrintError(L"Failed to read file %s "
					     L"(err: 0x%x)\n", FilePath,
					     Status);
			if (!*Data)
				EfiMemoryFree(FileBuffer);
			goto ErrOnReadFileBuffer;
		}
	}

	*Data = FileBuffer;
	*DataSize = FileSize;

	EfiConsolePrintDebug(L"File %s loaded (%d-byte)\n", FilePath,
			     FileSize);

ErrOnReadFileBuffer:
ErrOnAllocFileBuffer:
ErrOnReadFileInfo:
	FileHandle->Close(FileHandle);

ErrOnOpenFile:
	if (FilePath != Path)
		EfiMemoryFree(FilePath);

	return Status;
}

STATIC BOOLEAN
LoadSignatureRequired(CONST CHAR16 *Path)
{
	if (StrEndsWith(Path, L".p7a") == TRUE ||
	    StrEndsWith(Path, L".p7b") == TRUE
#ifdef EXPERIMENTAL_BUILD
	    || StrEndsWith(Path, L".p7s") == TRUE
#endif
	   )
		return FALSE;

	if (EfiSecurityPolicySecureBootEnabled() == FALSE) {
		EfiConsolePrintDebug(L"Ignore to verify file %s "
				     L"due to Secure Boot disabled\n",
				     Path);
		return FALSE;
	}

	return TRUE;
}

EFI_STATUS
EfiFileLoad(CONST CHAR16 *Path, VOID **Data, UINTN *DataSize)
{
	if (!Path)
		return EFI_INVALID_PARAMETER;

	EFI_STATUS Status, RealStatus;

	Status = EfiLibraryVectorizedBufferEnter(Data, DataSize);
	if (EFI_ERROR(Status))
		return Status;

	BOOLEAN CheckSignature;

	CheckSignature = LoadSignatureRequired(Path);
	EfiConsolePrintDebug(L"Signature verification is %srequired\n",
			     CheckSignature == TRUE ? L"" : L"not ");
	if (CheckSignature == FALSE) {
		Status = LoadFile(Path, NULL, Data, DataSize);
		/*
		 * Expect to extract the content from .p7a if the specified
		 * file doesn't exist.
		 */
		if (!EFI_ERROR(Status) || Status != EFI_NOT_FOUND)
			goto out;

		EfiConsolePrintDebug(L"Not found the file %s\n", Path);
	}

	EfiConsolePrintDebug(L"Attempting to load the content-attached "
			     L"signature file %s.p7a %s...\n", Path,
			     CheckSignature == TRUE ? L"" :
						      L"for the extracted "
						      L"content ");

	VOID *Signature = NULL;
	UINTN SignatureSize = 0;

	Status = LoadFile(Path, L".p7a", &Signature, &SignatureSize);
	RealStatus = Status;
	if (!EFI_ERROR(Status)) {
		BOOLEAN SaveContentRequired = TRUE;

		/*
		 * Save the content as long as the caller doesn't request
		 * the full content.
		 */
		if (Data && !*Data && !*DataSize)
			SaveContentRequired = FALSE;

		/*
		 * Even CheckSignature == FALSE, we have to verify .p7a
		 * to retrieve the extracted data.
		 */
		VOID *ExtractedData = NULL;
		UINTN ExtractedDataSize = 0;

		Status = EfiSignatureVerifyAttached(Signature, SignatureSize,
						    &ExtractedData,
						    &ExtractedDataSize);
		EfiMemoryFree(Signature);
		if (!EFI_ERROR(Status)) {
			/*
			 * The caller supplies the big enough buffer to store
			 * the full content.
			 */
			if (SaveContentRequired == TRUE && Data &&
			    *DataSize >= ExtractedDataSize)
				SaveContentRequired = FALSE;

			if (SaveContentRequired == TRUE)
				Status = EfiFileSave(Path, ExtractedData,
						     ExtractedDataSize);
			else
				Status = EfiLibraryVectorizedBufferLeave(Data,
									 DataSize,
									 ExtractedData,
									 ExtractedDataSize,
									 FALSE);

			EfiMemoryFree(ExtractedData);

			goto out;
		}
	}

	/*
	 * If .p7a doesn't exist too, there is no way to retrieve
	 * the file to load.
	 */
	if (CheckSignature == FALSE) {
		EfiConsolePrintError(L"Unable to load file %s\n",
				     Path);
		return Status;
	}

	EfiConsolePrintDebug(L"Attempting to load the attached signature "
			     L"file %s.p7b ...\n", Path);

	Status = LoadFile(Path, L".p7b", &Signature, &SignatureSize);
	if (!EFI_ERROR(Status)) {
		VOID *RealData = NULL;
		UINTN RealDataSize = 0;

		Status = LoadFile(Path, NULL, &RealData, &RealDataSize);
		if (EFI_ERROR(Status)) {
			EfiMemoryFree(Signature);
			EfiConsolePrintError(L"Failed to load the file "
					     L"%s for verifying .p7b (err: "
					     L"0x%x)\n", Path, Status);
			goto out;
		}

		Status = EfiSignatureVerifyAttached(Signature, SignatureSize,
						    &RealData, &RealDataSize);
		EfiMemoryFree(Signature);
		if (!EFI_ERROR(Status)) {
			Status = EfiLibraryVectorizedBufferLeave(Data,
								 DataSize,
								 RealData,
								 RealDataSize,
								 FALSE);
			if (!Data || *Data)
				EfiMemoryFree(RealData);

			goto out;
		}
	} else {
		if (Status == EFI_NOT_FOUND)
			Status = RealStatus;
		else
			RealStatus = Status;
	}

#ifdef EXPERIMENTAL_BUILD
	EfiConsolePrintDebug(L"Attempting to load the detached signature "
			     L"file %s.p7s ...\n", Path);

	Status = LoadFile(Path, L".p7s", &Signature, &SignatureSize);
	if (!EFI_ERROR(Status)) {
		VOID *RealData = NULL;
		UINTN RealDataSize = 0;

		Status = LoadFile(Path, NULL, &RealData, &RealDataSize);
		if (EFI_ERROR(Status)) {
			EfiMemoryFree(Signature);
			EfiConsolePrintError(L"Failed to load the file "
					     L"%s for verifying .p7s (err: "
					     L"0x%x)\n", Path, Status);
			goto out;
		}

		Status = EfiSignatureVerifyBuffer(Signature, SignatureSize,
						  RealData, RealDataSize);
		EfiMemoryFree(Signature);
		if (!EFI_ERROR(Status)) {
			Status = EfiLibraryVectorizedBufferLeave(Data,
								 DataSize,
								 RealData,
								 RealDataSize,
								 FALSE);
			if (!Data || *Data)
				EfiMemoryFree(RealData);

			goto out;
		}
	} else {
		if (Status == EFI_NOT_FOUND)
			Status = RealStatus;
		else
			RealStatus = Status;

		EfiConsolePrintDebug(L"Failed to load the signature file "
				     L"%s.p7a|.p7b|.p7s\n", Path);
	}
#else
	EfiConsolePrintDebug(L"Failed to load the signature file "
			     L"%s.p7a|.p7b\n", Path);
#endif

out:
	EfiConsoleTraceInfo(L"The file %s loaded with the exit code 0x%x\n",
			    Path, Status);

	return Status;
}

EFI_STATUS
EfiFileSave(CONST CHAR16 *Path, VOID *Data, UINTN DataSize)
{
	EFI_FILE_HANDLE FileHandle;
	EFI_STATUS Status;

	Status = OpenFile(Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
			  EFI_FILE_MODE_CREATE, &FileHandle, NULL);
	if (EFI_ERROR(Status))
		return Status;

	Status = FileHandle->Write(FileHandle, &DataSize, Data);
	FileHandle->Close(FileHandle);
	if (!EFI_ERROR(Status))
		EfiConsolePrintDebug(L"File %s written (%d-byte)\n", Path,
				     DataSize);
	else
		EfiConsolePrintError(L"Failed to write file %s "
				     L"(err: 0x%x)\n", Path, Status);

	return Status;
}

EFI_STATUS
EfiFileDelete(CONST CHAR16 *Path)
{
	EFI_FILE_HANDLE FileHandle;
	EFI_STATUS Status;

	Status = OpenFile(Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, &FileHandle,
			  NULL);
	if (EFI_ERROR(Status))
		return Status;

	Status = FileHandle->Delete(FileHandle);
	if (!EFI_ERROR(Status))
		EfiConsolePrintDebug(L"File %s deleted\n", Path);
	else
		EfiConsolePrintError(L"Failed to delete file %s "
				     L"(err: 0x%x)\n", Path, Status);

	return Status;
}
