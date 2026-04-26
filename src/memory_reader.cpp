#include "memory_reader.hpp"
#include "dbglog.hpp"
#include <Windows.h>
#include <cstdio>

ROState MemoryReader::read() {
    ROState st{};

    __try {
        const auto* off = ROOffsets::get();
        if (!off) {
            dbglog("[mem] unknown client_ver — cannot read AID/CID");
            return st;
        }

        auto* ptr_aid = reinterpret_cast<int*>(ROOffsets::base() + off->account_id);
        auto* ptr_cid = reinterpret_cast<int*>(ROOffsets::base() + off->char_id);

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ptr_aid, &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            st.account_id = *ptr_aid;
        }
        if (VirtualQuery(ptr_cid, &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            st.char_id = *ptr_cid;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dbglog("[mem] exception reading AID/CID");
        st = {};
    }

    st.auth_ready = (st.account_id > 0 && st.char_id > 0);
    st.valid      = st.auth_ready;

#ifdef VOICE_LOG
    static DWORD s_last_log = 0;
    DWORD now = GetTickCount();
    if (now - s_last_log >= 5000) {
        s_last_log = now;
        char b[64];
        sprintf_s(b, "[mem] acc=%d char=%d auth=%d",
            st.account_id, st.char_id, (int)st.auth_ready);
        dbglog(b);
    }
#endif

    return st;
}
