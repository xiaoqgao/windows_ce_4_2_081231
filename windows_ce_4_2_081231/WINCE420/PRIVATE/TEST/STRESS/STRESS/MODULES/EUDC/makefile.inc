!if 0
Copyright (c) Microsoft Corporation.  All rights reserved.
!endif
!if 0
This source code is licensed under Microsoft Shared Source License
Version 1.0 for Windows CE.
For a copy of the license visit http://go.microsoft.com/fwlink/?LinkId=3223.
!endif
!if "$(NOPASS0)"=="1"
dummy.txt:

!else

!if "$(WINCEREL)"=="1"

dummy.txt:
    copy test03.tte $(_FLATRELEASEDIR)
    copy test03.tte $(_RELEASEDIR)
!else

dummy.txt:
    copy test03.tte $(_RELEASEDIR)
!endif

!endif