
var LibraryEmulatorJS = {
    $EmulatorJSGetState: function() {
        let info = _save_state_info();
        let state = UTF8ToString(info).split("|");
        if (state[2] !== "1") {
            console.error("Failed to save state!", state[0]);
            throw new Error(state[0]);
        }
        const size = parseInt(state[0]);
        const dataStart = parseInt(state[1]);
        const data = HEAPU8.subarray(dataStart, dataStart + size);
        _free(info);
        _free(data);
        return new Uint8Array(data);
    },
    $EmulatorJSGetMemoryData: function(key) {
        let keyPtr = stringToNewUTF8(key);
        const info = _get_memory_data(keyPtr);
        _free(keyPtr);
        if (!info) return;
        const data_info = UTF8ToString(info).split("|");
        _free(info);
        const size = parseInt(data_info[0]);
        const dataStart = parseInt(data_info[1]);
        const data = HEAPU8.subarray(dataStart, dataStart + size);
        return data;
    },
};

addToLibrary(LibraryEmulatorJS);
