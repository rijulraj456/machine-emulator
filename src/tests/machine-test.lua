#!/usr/bin/env luapp5.3

-- Copyright 2019 Cartesi Pte. Ltd.
--
-- This file is part of the machine-emulator. The machine-emulator is free
-- software: you can redistribute it and/or modify it under the terms of the GNU
-- Lesser General Public License as published by the Free Software Foundation,
-- either version 3 of the License, or (at your option) any later version.
--
-- The machine-emulator is distributed in the hope that it will be useful, but
-- WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
-- FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
-- for more details.
--
-- You should have received a copy of the GNU Lesser General Public License
-- along with the machine-emulator. If not, see http://www.gnu.org/licenses/.
--
-- Note: hashes in the tests are used from rom.bin version v0.5.0
-- Changing ROM version will change the resulting hashes and invalidate test
local cartesi = require "cartesi"
local test_util = require "tests.util"


-- There is no UINT64_MAX in Lua, so we have to use the signed representation
local MAX_MCYCLE = -1

local pmas_file_names = {
    "0000000000000000--0000000000001000.bin",
    "0000000000001000--000000000000f000.bin",
    "0000000002000000--00000000000c0000.bin",
    "0000000040008000--0000000000001000.bin",
    "0000000080000000--0000000000100000.bin"
}

local function parse_pma_file(filename)
    local fd = io.open(filename, "rb")
    local data_size = fd:seek("end")
    fd:seek("set")
    local data = fd:read(data_size)
    fd:close(filename)
    return {data_size=data_size, data=data}
end

local function run_loop(machine, mcycle_end)
    while machine:read_mcycle() < mcycle_end do
        machine:run(mcycle_end)
        if machine:read_iflags_H() then break end
    end
end

local function build_machine()
    local cpu_addr = {}


    -- Use default config to be max reproducible
    local machine = cartesi.machine {
        processor = cpu_addr,
        ram = {length = 1 << 20},
        rom = {image_filename = test_util.images_path .. "rom.bin"}
    }

    return machine
end

local function build_machine_with_flash()
    local cpu_addr = {}

    flash_drive_config = {

        start = 0x8000000000000000,
        length = 0x3c00000,
        shared = false,
        image_filename = test_util.images_path .. "rootfs.ext2"
    }

    -- Use default config to be max reproducible
    local machine = cartesi.machine {
        processor = cpu_addr,
        ram = {length = 1 << 20},
        rom = {image_filename = test_util.images_path .. "rom.bin"},
        flash_drive = {flash_drive_config}
    }

    return machine
end

local do_test = test_util.make_do_test(build_machine)
local do_test_with_flash = test_util.make_do_test(build_machine_with_flash)

local function remove_files(file_names)
    for _, file_name in pairs(file_names) do os.remove(file_name) end
end

-- Take data from dumped memory files
-- and calculate root hash of the machine
local function calculate_emulator_hash(pmas_files)

    -- Read Data
    local procesor_board_shadow = parse_pma_file(pmas_files[1])
    local rom = parse_pma_file(pmas_files[2])
    local cli = parse_pma_file(pmas_files[3])
    local hti = parse_pma_file(pmas_files[4])
    local ram = parse_pma_file(pmas_files[5])
    
    local cpu_and_rom_data = procesor_board_shadow.data .. rom.data
    local cpu_and_rom_data_pages = (procesor_board_shadow.data_size +
                                       rom.data_size) / (2 ^ 12)
    local cpu_and_rom_space_hash = test_util.calculate_region_hash(
                                       cpu_and_rom_data, cpu_and_rom_data_pages,
                                       12, 16)
    cpu_and_rom_space_hash = test_util.extend_region_hash(
                                 cpu_and_rom_space_hash, 0x0, 16, 25)

    local cli_space_hash = test_util.calculate_region_hash(cli.data,
                                                            cli.data_size /
                                                                (2 ^ 12), 12, 14)
    cli_space_hash = test_util.extend_region_hash(cli_space_hash, 0x02000000,
                                                   14, 25)

    local cpu_rom_cli_hash = cartesi.keccak(cpu_and_rom_space_hash,
                                            cli_space_hash) -- 26
    cpu_rom_cli_hash = test_util.extend_region_hash(cpu_rom_cli_hash, 0x0, 26,
                                                     30)

    local hti_log2_data_size = math.log(hti.data_size, 2)
    local hti_space_hash = test_util.calculate_region_hash_2(0x40008000,
                                                              hti.data,
                                                              hti_log2_data_size,
                                                              30)

    local ram_log2_data_size = math.log(ram.data_size, 2)
    local ram_space_hash = test_util.calculate_region_hash_2(0x80000000,
                                                              ram.data,
                                                              ram_log2_data_size,
                                                              31)

    local left = cartesi.keccak(cpu_rom_cli_hash, hti_space_hash) -- 31
    local used_space_hash = cartesi.keccak(left, ram_space_hash) -- 32
    local total_space_hash = test_util.extend_region_hash(used_space_hash, 0x0,
                                                           32, 64)

    return total_space_hash
end

print("\n\ntesting getting machine intial config and iflags")
do_test("machine halt and yield flags and config matches", 
    function(machine)
        -- Get machine default config  and test for known fields
        local initial_config = machine:get_initial_config()
        -- test_util.print_table(initial_config)
        assert(initial_config["processor"]["marchid"] == 7,
            "marchid value does not match")
        assert(initial_config["processor"]["pc"] == 0x1000,
            "pc value does not match")
        assert(initial_config["ram"]["length"] == 1048576,
            "ram length value does not match")
        assert(initial_config["rom"]["image_filename"] ~= "",
            "rom image filename not set")
        -- Check machine is not halted
        assert(not machine:read_iflags_H(), "machine shouldn't be halted")
        -- Check machine is not yielded
        assert(not machine:read_iflags_Y(), "machine shouldn't be yielded")
    end
)

print("\n\ntesting memory dump to files")
do_test("dumped file merkle tree hashes should match", 
    function(machine)
        -- Dump memory regions to files
        -- Calculate merkle tree hash for all existing memory regions
        -- Check for expected values
        local root_file_hashes = {}
        machine:dump_pmas()

        for _, file_name in pairs(pmas_file_names) do

            local temp = test_util.split_string(file_name, "--.")
            local data_region_size = tonumber(temp[2], 16)
            local page_log2_size = 12
            local data_number_of_pages = data_region_size / (2 ^ page_log2_size)
            local tree_log2_size = math.ceil(math.log(data_region_size, 2))

            local pmas_file = parse_pma_file(file_name)

            root_file_hashes[file_name] = test_util.calculate_region_hash(pmas_file.data,
                                                                        data_number_of_pages,
                                                                        page_log2_size,
                                                                        tree_log2_size)
            print("\nHash of address space: ", file_name, " of log size:",
                tree_log2_size, " is ",
                test_util.tohex(root_file_hashes[file_name]))
        end
        assert(test_util.tohex(root_file_hashes[pmas_file_names[1]]) ==
                "E0387504AF856C8FDD164CA2EB44FEA5BC4094232D86C86021450687C9180863")
        assert(test_util.tohex(root_file_hashes[pmas_file_names[2]]) ==
                "3DF875E72A8B29E6F0DB9CFE2934F7B80E7DB855C1729CF140D358A72B9D73BD")
        assert(test_util.tohex(root_file_hashes[pmas_file_names[3]]) ==
                "995C871A78EFEC6CA5AFD44B9994B1C88BBBFCDFEA68FD5566C13D4F45BBDE6B")
        assert(test_util.tohex(root_file_hashes[pmas_file_names[4]]) ==
                "3279ED2C35ADE5BCFFC6680AA2E08153D2EDD5A9949ECA9E731B3F5DCE2721A0")
        assert(test_util.tohex(root_file_hashes[pmas_file_names[5]]) ==
                "99AF665835AABFDC6740C7E2C3791A31C3CDC9F5AB962F681B12FC092816A62F")

        remove_files(pmas_file_names)
    end
)

print("\n\ntesting if machine initial hash is correct")
do_test("machine initial hash shold match", 
    function(machine)
        -- Update merkle tree
        machine:update_merkle_tree()

        -- Get starting root hash
        local root_hash = machine:get_root_hash()

        machine:dump_pmas()
        local calculated_root_hash = calculate_emulator_hash(pmas_file_names,
                                                            machine)

        print("Root hash:", test_util.tohex(root_hash), " calculated root hash:",
            test_util.tohex(calculated_root_hash))

        assert(
            test_util.tohex(root_hash) == test_util.tohex(calculated_root_hash),
            "Initial root hash does not matches")

        remove_files(pmas_file_names)
    end
)

print("\n\ntesting root hash after step one")
do_test("machine root hash after step one shold match", 
    function(machine)

        -- Update merkle tree
        machine:update_merkle_tree()

        -- Get starting root hash
        local root_hash = machine:get_root_hash()
        assert(test_util.tohex(root_hash) ==
                "718D7B97C245EF4F12E67576C5CD8F91EE92028181BD7341E5E7ECF8242DA5B8",
            "hash after initial step does not match")

        -- Perform step, dump address space to file, calculate emulator root hash
        -- and check if maches
        local log_type = {}
        machine:step(log_type)
        machine:update_merkle_tree()
        local root_hash_step1 = machine:get_root_hash()

        machine:dump_pmas()
        local calculated_root_hash_step1 = calculate_emulator_hash(pmas_file_names,
                                                                machine)

        -- Remove dumped pmas files
        remove_files(pmas_file_names)

        assert(test_util.tohex(root_hash_step1) ==
                test_util.tohex(calculated_root_hash_step1),
            "hash after first step does not match")
    end
)

print("\n\ntesting proof after step one")
do_test("proof check should pass", 
    function(machine)

        machine:update_merkle_tree()

        local log_type = {}
        machine:step(log_type)
        machine:update_merkle_tree()

        -- Dump RAM memory to file, calculate hash of file
        -- get proof of ram using get_proof and check if 
        -- hashes match
        machine:dump_pmas()
        local ram_file_name = pmas_file_names[5]
        local ram = parse_pma_file(ram_file_name)

        remove_files(pmas_file_names)

        local ram_address_start = tonumber(test_util.split_string(ram_file_name,
                                                                "--.")[1], 16)
        local ram_data_number_of_pages = ram.data_size / (2 ^ 12)
        local ram_log2_data_size = math.log(ram.data_size, 2)
        local calculated_ram_hash = test_util.calculate_region_hash(ram.data,
                                                                    ram_data_number_of_pages,
                                                                    12,
                                                                    ram_log2_data_size)
        local ram_proof = machine:get_proof(ram_address_start, ram_log2_data_size)
        local root_hash = machine:get_root_hash()

        assert(test_util.tohex(root_hash) == test_util.tohex(ram_proof.root_hash),
            "root hash in proof does not match")
        print("target hash:", test_util.tohex(ram_proof.target_hash),
            " calculated target hash:", test_util.tohex(calculated_ram_hash))
        assert(test_util.tohex(calculated_ram_hash) ==
                test_util.tohex(ram_proof.target_hash),
            "target hash in proof does not match")
    end
)

print("\n\nrun machine to 1000 mcycle and check for mcycle and root hash")
do_test("mcycle and root hash should match", 
    function(machine)
        -- Run to 1000 cycle tics
        local current_mcycle = machine:read_mcycle()
        while current_mcycle < 1000 do
            machine:run(1000)
            current_mcycle = machine:read_mcycle()
        end

        assert(machine:read_mcycle() == 1000, "machine mcycle should be 1000")

        machine:update_merkle_tree()
        local root_hash = machine:get_root_hash()
        print("1000 cycle hash: ", test_util.tohex(root_hash))
        assert(test_util.tohex(root_hash) ==
                "CB64669FF4E279E0BF536BB593153B61C7BF95438F3C7092C49F806C684A34EA",
            "machine hash does not match after 1000 cycles")
    end
)

print("\n\nrun machine to end mcycle and check for mcycle, hash and halt flag")
do_test("mcycle and root hash should match", 
    function(machine)
        machine:run(MAX_MCYCLE)
        -- Check machine is halted
        assert(machine:read_iflags_H(), "machine should be halted")
        -- Check for end mcycle
        local end_mcycle = machine:read_mcycle()
        assert(end_mcycle > 400000, "machine mcycle should be above 400000")

        machine:update_merkle_tree()
        local root_hash = machine:get_root_hash()
        print("End hash: ", test_util.tohex(root_hash))
        assert(test_util.tohex(root_hash) ==
                "FF2C58BDBAF475BF35FBA52883C31E8912B5DAE3C214BB76C8DD133B2136E546",
            "machine hash does not match after on end cycle")
    end
)

print("\n\nwrite something to ram memory and check if hash and proof matches")
do_test("proof  and root hash should match", 
    function(machine)
        machine:update_merkle_tree()
        local root_hash = machine:get_root_hash()
        local ram_address_start = 0x80000000

        -- Find proof for first KB of ram
        local initial_ram_proof = machine:get_proof(ram_address_start, 10)
        -- Calculate hash
        local initial_memory_read = machine:read_memory(ram_address_start, 2 ^ 10)
        local initial_calculated_hash = test_util.calculate_root_hash(
                                            initial_memory_read, 10)
        assert(test_util.tohex(initial_ram_proof.target_hash) ==
                test_util.tohex(initial_calculated_hash),
            "initial hash does not match")

        print("initial target hash:",
            test_util.tohex(initial_ram_proof.target_hash),
            " calculated initial target hash:",
            test_util.tohex(initial_calculated_hash))

        machine:write_memory(0x8000000F, "mydataol12345678", 0x10)

        machine:update_merkle_tree()
        local root_hash_step1 = machine:get_root_hash()
        local verify = machine:verify_merkle_tree()

        -- Find proof for first KB of ram
        local ram_proof = machine:get_proof(ram_address_start, 10)
        -- Calculate hash
        local memory_read = machine:read_memory(ram_address_start, 2 ^ 10)
        local calculated_hash = test_util.calculate_root_hash(memory_read, 10)

        print("end target hash:", test_util.tohex(ram_proof.target_hash),
            " calculated end target hash:", test_util.tohex(calculated_hash))

        assert(test_util.tohex(initial_ram_proof.target_hash) ~=
                test_util.tohex(ram_proof.target_hash),
            "hash is same after memory is written")

        assert(test_util.tohex(initial_calculated_hash) ~=
                test_util.tohex(calculated_hash),
            "calculated hash is same after memory is written")

        assert(test_util.tohex(ram_proof.target_hash) ==
                test_util.tohex(calculated_hash),
            "hash does not match after memory is written")
    end
)

print("\n\n check dirty page maps")
do_test("dirty page maps should be consistent", 
    function(machine)
        -- Verify dirty page maps
        assert(machine:verify_dirty_page_maps(), "error verifying dirty page maps")
    end
)

print("\n\n check replace flash drives")
do_test_with_flash("should replace flash drive and read something",
    function(machine)
        -- Create temp flash file
        local p = io.popen(
                    [[ echo "test data 1234567890" > input.raw && truncate -s 62914560 input.raw]])
        p:close()

        local initial_config = machine:get_initial_config()

        local flash_address_start = 0x8000000000000000
        flash_drive_config = {
            start = flash_address_start,
            length = 0x3c00000,
            image_filename = "input.raw",
            shared = true
        }

        local flash_data = machine:read_memory(flash_address_start, 20)

        machine:replace_flash_drive(flash_drive_config)

        local flash_data = machine:read_memory(flash_address_start, 20)
        assert(flash_data == "test data 1234567890", "data read from replaced flash failed")
        os.remove("input.raw")
    end
)

print("\n\n check for relevant register values after step 1")
do_test("register values should match", 
    function(machine)
        local pc_before = machine:read_pc()
        local minstret_before = machine:read_minstret()
        local mcycle_before = machine:read_mcycle()
        
        local log_type = {}
        machine:step(log_type)

        local pc_after = machine:read_pc()
        local minstret_after = machine:read_minstret()
        local mcycle_after = machine:read_mcycle()

        assert(pc_before + 4 == pc_after, "wrong pc value")
        assert(minstret_before + 1 == minstret_after, "wrong minstret value")
        assert(mcycle_before + 1 == mcycle_after, "wrong mcycle value")
    end
)

print("\n\nAll tests of machine lua API passed")
