#include "746FlashSim.h"
#include <set>

uint8_t SSD_SIZE;
uint8_t PACKAGE_SIZE;
uint16_t DIE_SIZE;
uint16_t PLANE_SIZE;
uint16_t BLOCK_SIZE;
uint16_t BLOCK_ERASES;
uint8_t OVERPROVISIONING;

size_t RAW_CAPACITY;
size_t ADDRESSABLE;
size_t empty_log_block;
size_t clean_ptr;
uint16_t clean_erases = 0;
uint16_t EXCHANGE;
uint8_t CLEAN_NUM = 10;

std::set<size_t> written;
std::set<size_t> garbage;

std::map<size_t, size_t> logic2data_map;
std::map<size_t, uint16_t> block_erases; // <log_block_id, erases_num>

template<typename PageType>
class MyFTL : public FlashSim::FTLBase<PageType> {

public:

    class LogBlock;
    std::map<size_t, LogBlock*> data2log_map; // <data_block_id, LogBlock *>
    std::set<LogBlock*> freed_log; // The logs that have been unmapped.

    // A class for each log block.
    class LogBlock {
    public:
        uint8_t package;
        uint8_t die;
        uint16_t plane;
        uint16_t block;
        size_t block_id;
        uint16_t empty_page_start;
        std::map<uint16_t, uint16_t> log_page_map;
        bool isEmpty;

        LogBlock(FlashSim::Address *addr, size_t block_id) {
            package = addr->package;
            die = addr->die;
            plane = addr->plane;
            block = addr->block;
            this->block_id = block_id;
            empty_page_start = 0;
            isEmpty = false;
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> read(
                FlashSim::Address addr) {
            std::map<uint16_t, uint16_t>::iterator it = log_page_map.find
                    (addr.page);
            if (it != log_page_map.end()) {
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                                      FlashSim::Address(package, die, plane, block,
                                                        it->second));
            } else {
                return std::make_pair(FlashSim::ExecState::SUCCESS, addr);
            }
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> write(
                FlashSim::Address addr, size_t original_lba) {
            if (empty_page_start == BLOCK_SIZE) {
                return std::make_pair(FlashSim::ExecState::FAILURE,
                                      FlashSim::Address(0, 0, 0, 0, 0));
            }
            // Update the page log.
            std::map<uint16_t, uint16_t>::iterator it = log_page_map.find(
                    addr.page);
            if (garbage.find(original_lba) != garbage.end()) {
                garbage.erase(original_lba);
            }
            if (it != log_page_map.end()) {
                it->second = empty_page_start;
            } else {
                log_page_map.insert(std::make_pair(addr.page, empty_page_start));
            }
            return std::make_pair(FlashSim::ExecState::SUCCESS,
                                  FlashSim::Address(package, die, plane, block,
                                                    empty_page_start++));
        }
    };

    // Physical location information of a lba.
    class DataBlockInfo {
    public:
        size_t id; // data block id
        FlashSim::Address addr;
        DataBlockInfo(size_t id, FlashSim::Address addr) {
            this->id = id;
            this->addr = addr;
        }
    };

    // Get the physical location given a lba.
    DataBlockInfo getDataBlock(size_t lba) {
        FlashSim::Address original_addr = lba2addr(lba);
        if (logic2data_map.find(lba / BLOCK_SIZE) == logic2data_map.end()) {
            return DataBlockInfo(lba / BLOCK_SIZE, original_addr);
        } else {
            size_t data_block_id = logic2data_map.find(lba / BLOCK_SIZE)->second;
            FlashSim::Address new_addr = lba2addr(data_block_id * BLOCK_SIZE);
            new_addr.page = original_addr.page;
            return DataBlockInfo(data_block_id, new_addr);
        }
    }

    // Return the lba given the physical block id and page number
    size_t getLba(size_t data_block_id, uint16_t page) {
        std::map<size_t, size_t>::iterator it;
        for (it = logic2data_map.begin(); it != logic2data_map.end(); it++) {
            if (it->second == data_block_id) {
                return it->first * BLOCK_SIZE + page;
            }
        }
        return data_block_id * BLOCK_SIZE + page;
    }

    // Get the erase count given the block id.
    uint16_t getEraseNum(size_t block_id) {
        std::map<size_t, uint16_t>::iterator it = block_erases.find(block_id);
        if (it != block_erases.end()) {
            return it->second;
        } else {
            return 0;
        }

    }

    // Increase erase count by one.
    void increaseErase(size_t block_id) {
        std::map<size_t, uint16_t>::iterator it = block_erases.find(block_id);
        if (it == block_erases.end()) {
            block_erases.insert(std::make_pair(block_id, 1));
        } else {
            it->second++;
        }
    }

    // Reset the elements of a certain log block.
    void resetLogBlock(LogBlock *log_block) {
        log_block->empty_page_start = 0;
        log_block->log_page_map.clear();
    }

    // Check if the log has all the valid pages in a data block.
    bool logHasAll(size_t data_block_id, LogBlock* log_block) {
        for (int16_t i = 0; i < BLOCK_SIZE; i++) {
            if (written.find(getLba(data_block_id, i)) != written.end()
                && garbage.find(getLba(data_block_id, i)) == garbage.end()
                && log_block->log_page_map.find(i) == log_block->log_page_map.end()) {
                return false;
            }
        }
        return true;
    }

    // Change the empty log block to a given block id.
    void changeLog2DataBlock(size_t data_block_id, bool empty, LogBlock*
    log_block) {
        resetLogBlock(log_block);
        FlashSim::Address data_block_addr = lba2addr(data_block_id *
                                                     BLOCK_SIZE);
        log_block->package = data_block_addr.package;
        log_block->die = data_block_addr.die;
        log_block->plane = data_block_addr.plane;
        log_block->block = data_block_addr.block;
        log_block->block_id = data_block_id;
        if (empty) {
            block_erases.insert(std::make_pair(data_block_id, 0));
            logic2data_map.insert(std::make_pair(data_block_id,
                                                 log_block->block_id));
            log_block->isEmpty = true;
        }
    }

//    size_t changeDataBlock()

    /*
     * Map LBA into PBA.
     * Return FlashSim::Address
     */
    FlashSim::Address lba2addr(size_t lba) {
        size_t tmp1 = (size_t) PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * BLOCK_SIZE;
        uint8_t package = lba / tmp1;
        lba = lba % tmp1;

        size_t tmp2 = (size_t) DIE_SIZE * PLANE_SIZE * BLOCK_SIZE;
        uint8_t die = lba / tmp2;
        lba = lba % tmp2;

        uint32_t tmp3 = (uint32_t) PLANE_SIZE * BLOCK_SIZE;
        uint16_t plane = lba / tmp3;
        lba = lba % tmp3;

        uint16_t block = lba / BLOCK_SIZE;
        uint16_t page = lba % BLOCK_SIZE;

        return FlashSim::Address(package, die, plane, block, page);
    }

    /*
     * Constructor
     */
    MyFTL(const FlashSim::Configuration *conf) {
        SSD_SIZE = conf->GetInteger("SSD_SIZE");
        PACKAGE_SIZE = conf->GetInteger("PACKAGE_SIZE");
        DIE_SIZE = conf->GetInteger("DIE_SIZE");
        PLANE_SIZE = conf->GetInteger("PLANE_SIZE");
        BLOCK_SIZE = conf->GetInteger("BLOCK_SIZE");
        BLOCK_ERASES = conf->GetInteger("BLOCK_ERASES");
        OVERPROVISIONING = conf->GetInteger("OVERPROVISIONING");
        RAW_CAPACITY = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE
                       * BLOCK_SIZE;
        ADDRESSABLE = RAW_CAPACITY - RAW_CAPACITY * OVERPROVISIONING / 100;
        empty_log_block = ADDRESSABLE;
        clean_ptr = RAW_CAPACITY / BLOCK_SIZE;
        EXCHANGE = BLOCK_ERASES - 1;
    }

    /*
     * Destructor - Plase keep it as virtual to allow destroying the
     *              object with base type pointer
     */
    virtual ~MyFTL() {
    }

    /*
     * ReadTranslate() - Translates read address
     *
     * This function translates a physical LBA into an Address object that will
     * be used as the target address of the read operation.
     *
     * If you need to issue extra operations, please use argument func to
     * interact with class Controller
     */
    std::pair<FlashSim::ExecState, FlashSim::Address>
    ReadTranslate(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        if (written.find(lba) == written.end()) {
            // The page has never been written before.
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        if (lba >= ADDRESSABLE) {
            // Invalid lba
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        if (garbage.find(lba) != garbage.end()) {
            // The page has already been garbage.
            std::cout << "booo: The page has already become garbage!\n";
//            return std::make_pair(FlashSim::ExecState::FAILURE,
//                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        DataBlockInfo data_block = getDataBlock(lba);
        typename std::map<size_t, LogBlock *>::iterator it = data2log_map
                .find(data_block.id);
        if (it == data2log_map.end()) {
            // No log-reservation block mapped to this data block.
            return std::make_pair(FlashSim::ExecState::SUCCESS, data_block.addr);
        } else {
            // Check if there is a more recent copy in log-reservation blocks.
            LogBlock *logBlock = it->second;
            return logBlock->read(data_block.addr);
        }
    }

    /*
     * WriteTranslate() - Translates write address
     *
     * Please refer to ReadTranslate()
     */
    std::pair<FlashSim::ExecState, FlashSim::Address>
    WriteTranslate(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        // Invalid lba
        if (lba >= ADDRESSABLE) {
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
        DataBlockInfo data_block = getDataBlock(lba);
        if (written.find(lba) != written.end()) {
            // The page is not empty
            typename std::map<size_t, LogBlock *>::iterator it = data2log_map
                    .find(data_block.id);
            if (it == data2log_map.end()) { // No log block mapped to it.
                if (empty_log_block >= (RAW_CAPACITY - BLOCK_SIZE * CLEAN_NUM)) {
                    // Log-reservation blocks are all mapped.
                    return noEmptyLogBlocks(func, data_block.id, data_block
                            .addr, lba);
                } else {
                    // Map a new log-reservation block to the data block.
                    FlashSim::Address new_log_block = lba2addr(empty_log_block);
                    LogBlock *logBlock = new LogBlock(&new_log_block, empty_log_block / BLOCK_SIZE);
                    empty_log_block += BLOCK_SIZE;
                    data2log_map.insert(std::make_pair(data_block.id,
                                                       logBlock));
                    block_erases.insert(std::make_pair(logBlock->block_id, 0));
                    return logBlock->write(data_block.addr, lba);
                }
            } else {
                // There's a log-reservation block mapped to this data block.
                LogBlock *logBlock = it->second;
                std::pair<FlashSim::ExecState, FlashSim::Address> result =
                        logBlock->write(data_block.addr, lba);
                if (result.first == FlashSim::ExecState::FAILURE) {
                    return noEmptyPages(func, data_block.id, data_block.addr);
                }
                return result;
            }
        } else {
            // The page is empty
            written.insert(lba);
            if (block_erases.find(data_block.id) == block_erases.end()) {
                block_erases.insert(std::make_pair(data_block.id, 0));
            }
            return std::make_pair(FlashSim::ExecState::SUCCESS, data_block.addr);
        }
    }

    // Move cleaning pointer to the next empty one if possible
    bool moveCleanPtr() {
        if (clean_ptr <= (RAW_CAPACITY / BLOCK_SIZE - CLEAN_NUM)) {
            return false;
        } else {
            std::cout << "Move clean_ptr " << clean_ptr
                      << "->" << clean_ptr - 1 << "\n";
            clean_ptr --;
            clean_erases = 0;
            return true;
        }
    }

    // Exchange cleaning block with the data block who has the largest
    // remaining life. Return true if such data block can be found.
    bool changeCleaningBlock(const
                             FlashSim::ExecCallBack<PageType> &func, size_t
                             clean_data_block_id) {
        if (clean_erases == BLOCK_ERASES) return false;
//        size_t data_block_id;
//        uint16_t min_erase = BLOCK_ERASES;
        for (size_t i = 0; i < ADDRESSABLE / BLOCK_SIZE; i++) {
            if (i == clean_ptr || i == clean_data_block_id) continue;
            if (block_erases.find(i) == block_erases.end()) {
                // The data block hasn't been written before.
                std::cout << "Found an empty data block! Perform exchange! "
                        "clean_ptr " << clean_ptr
                          << " changed to: " << i << "\n";
                logic2data_map.insert(std::make_pair(i, clean_ptr));
                FlashSim::Address old_cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
                func(FlashSim::OpCode::ERASE, old_cleaning_addr);
                if (block_erases.find(clean_ptr) != block_erases.end()) {
                    block_erases.find(clean_ptr)->second = clean_erases + 1;
                } else {
                    block_erases.insert(std::make_pair(clean_ptr,
                                                       clean_erases + 1));
                }
                block_erases.insert(std::make_pair(i, 0));
                clean_ptr = i;
                clean_erases = 0;
                return true;
            }
//            std::cout << getEraseNum(i);
//            if (getEraseNum(i) < min_erase) {
//                min_erase = getEraseNum(i);
//                data_block_id = i;
//                if (min_erase == 0) break;
//            }
        }
        return false;
//        if (min_erase == BLOCK_ERASES) {
//            // Cannot find a suitable data block.
//            std::cout << "Data blocks are all worn out\n";
//            return false;
//        }
//        std::cout << "\nFound data block " << data_block_id << " erases: "
//                  << getEraseNum(data_block_id) << ". Exchange with "
//                  << clean_ptr << std::endl;
//        FlashSim::Address data_block_start_addr = lba2addr(data_block_id * BLOCK_SIZE);
//        FlashSim::Address old_cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
//        func(FlashSim::OpCode::ERASE, old_cleaning_addr);
//        if (block_erases.find(clean_ptr) != block_erases.end()) {
//            block_erases.erase(clean_ptr);
//        }
//        block_erases.insert(std::make_pair(clean_ptr, clean_erases + 1));
//        std::cout << "Start copy!\n";
//        if (data2log_map.find(data_block_id) == data2log_map.end()) {
//            std::cout << "if\n";
//            // No log block mapped to this data block.
//            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
//                if (written.find(getLba(data_block_id, i)) != written.end()) {
//                    if (garbage.find(getLba(data_block_id, i)) != garbage.end()) {
//                        garbage.erase(getLba(data_block_id, i));
//                        written.erase(getLba(data_block_id, i));
//                        continue;
//                    }
//                    func(FlashSim::OpCode::READ,
//                         FlashSim::Address(data_block_start_addr.package,
//                                           data_block_start_addr.die,
//                                           data_block_start_addr.plane,
//                                           data_block_start_addr.block,
//                                           i));
//                    func(FlashSim::OpCode::WRITE,
//                         FlashSim::Address(old_cleaning_addr.package,
//                                           old_cleaning_addr.die,
//                                           old_cleaning_addr.plane,
//                                           old_cleaning_addr.block,
//                                           i));
//                }
//            }
//        } else {
//            std::cout << "else\n";
//            LogBlock* log_block = data2log_map.find(data_block_id)->second;
//            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
//                if (written.find(getLba(data_block_id, i)) != written.end()) {
//                    if (garbage.find(getLba(data_block_id, i)) != garbage.end()) {
//                        garbage.erase(getLba(data_block_id, i));
//                        written.erase(getLba(data_block_id, i));
//                        continue;
//                    }
//                    std::map<uint16_t, uint16_t>::iterator it2 =
//                            log_block->log_page_map.find(i);
//                    if (it2 == log_block->log_page_map.end()) {
//                        func(FlashSim::OpCode::READ,
//                             FlashSim::Address(data_block_start_addr.package,
//                                               data_block_start_addr.die,
//                                               data_block_start_addr.plane,
//                                               data_block_start_addr.block,
//                                               i));
//                    } else {
//                        func(FlashSim::OpCode::READ,
//                             FlashSim::Address(log_block->package,
//                                               log_block->die,
//                                               log_block->plane,
//                                               log_block->block,
//                                               it2->second));
//                    }
//                    func(FlashSim::OpCode::WRITE,
//                         FlashSim::Address(old_cleaning_addr.package,
//                                           old_cleaning_addr.die,
//                                           old_cleaning_addr.plane,
//                                           old_cleaning_addr.block,
//                                           i));
//                }
//            }
//            resetLogBlock(log_block);
//            if (getEraseNum(log_block->block_id) < BLOCK_ERASES) {
//                freed_log.insert(log_block);
//            }
//            data2log_map.erase(data_block_id);
//        }
//        std::map<size_t, size_t>::iterator it;
//        for (it = logic2data_map.begin(); it != logic2data_map.end(); it++) {
//            if (it->second == data_block_id) {
//                it->second = clean_ptr;
//                std::cout << "update!\n";
//            }
//        }
//        if (logic2data_map.find(data_block_id) == logic2data_map.end()) {
//            logic2data_map.insert(std::make_pair(data_block_id, clean_ptr));
//            std::cout << "insert!\n";
//        }
//        clean_ptr = data_block_id;
//        clean_erases = min_erase;
//        std::cout << "Exchange cleaning block done!\n";
//        std::cout << "Current logic2data map: \n";
//        typename std::map<size_t, size_t >::iterator t;
//        for (t = logic2data_map.begin(); t != logic2data_map.end(); t++) {
//            std::cout << t->first << "->" << t->second <<
//                      std::endl;
//        }
    }

//    size_t changeDataBLock(const
//                           FlashSim::ExecCallBack<PageType> &func) {
//
//    }

    // Perform cleaning given the data block and log block and whether a
    // cleaning block is needed.
    void standardCleaning(size_t data_block_id, LogBlock* log_block, const
    FlashSim::ExecCallBack<PageType> &func, bool need_cleaning_block, bool
                          erase_clean) {
        FlashSim::Address data_addr = lba2addr(data_block_id * BLOCK_SIZE);
        FlashSim::Address cleaning_addr;
        size_t new_data_block_id = data_block_id;
        FlashSim::Address new_data_addr = data_addr;
        std::cout << "Enter standardCleaning: data_block_id " << data_block_id
                  << ", log_block_id " << log_block->block_id
                  << ", need clean_block? " << need_cleaning_block
                  << ", clean_ptr " << clean_ptr
                  << ", clean_erases: " << clean_erases
                  << ", data2log size: " << data2log_map.size()
                  << ", logic2data size: " << logic2data_map.size()
                  <<std::endl;

        if (clean_ptr == RAW_CAPACITY / BLOCK_SIZE) {
            clean_ptr--;
            cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
        } else {
            cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
            if (need_cleaning_block && erase_clean) {
                func(FlashSim::OpCode::ERASE, cleaning_addr);
                clean_erases++;
            }
        }

        bool exchange = false;
        if (getEraseNum(data_block_id) >= EXCHANGE) {
            for (size_t j = (ADDRESSABLE / BLOCK_SIZE - 1); j >= 0; j--) {
                if (j == clean_ptr) continue;
                if (block_erases.find(j) == block_erases.end()) {
                    // The data block hasn't been written before.
                    std::cout << "Found an empty data block! Perform exchange! "
                            "data_block " << data_block_id
                              << " changed to: " << j << "\n";
                    exchange = true;
                    new_data_block_id = j;
                    new_data_addr = lba2addr(new_data_block_id * BLOCK_SIZE);
                    block_erases.insert(std::make_pair(j, 0));
                    break;
                }
                if (j==0) break;
            }
        }

        if (need_cleaning_block) {
            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                // Only save the page that has been written before
                if (written.find(getLba(data_block_id, i)) != written.end()) {
                    if (garbage.find(getLba(data_block_id, i)) != garbage.end()) {
                        garbage.erase(getLba(data_block_id, i));
                        written.erase(getLba(data_block_id, i));
                        continue;
                    }
                    std::map<uint16_t, uint16_t>::iterator it2 =
                            log_block->log_page_map.find(i);
                    if (it2 == log_block->log_page_map.end()) {
                        // No copy in the log block.
                        func(FlashSim::OpCode::READ,
                             FlashSim::Address(data_addr.package,
                                               data_addr.die, data_addr.plane,
                                               data_addr.block, i));
                    } else {
                        func(FlashSim::OpCode::READ,
                             FlashSim::Address(log_block->package,
                                               log_block->die, log_block->plane,
                                               log_block->block, it2->second));
                    }
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(cleaning_addr.package,
                                           cleaning_addr.die, cleaning_addr.plane,
                                           cleaning_addr.block, i));
                }
            }

            func(FlashSim::OpCode::ERASE, data_addr);

            // Copy live pages back to the data block
            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                if (written.find(getLba(data_block_id, i)) != written.end()) {
                    func(FlashSim::OpCode::READ,
                         FlashSim::Address(cleaning_addr.package,
                                           cleaning_addr.die, cleaning_addr.plane,
                                           cleaning_addr.block, i));
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(new_data_addr.package,
                                           new_data_addr.die, new_data_addr.plane,
                                           new_data_addr.block, i));
                }
            }
        } else {
            func(FlashSim::OpCode::ERASE, data_addr);
            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                // Only save the page that has been written before
                if (written.find(getLba(data_block_id, i)) != written.end()) {
                    if (garbage.find(getLba(data_block_id, i)) != garbage.end()) {
                        garbage.erase(getLba(data_block_id, i));
                        written.erase(getLba(data_block_id, i));
                        continue;
                    }
                    std::map<uint16_t, uint16_t>::iterator it2 =
                            log_block->log_page_map.find(i);
                    func(FlashSim::OpCode::READ,
                         FlashSim::Address(log_block->package,
                                           log_block->die, log_block->plane,
                                           log_block->block, it2->second));
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(new_data_addr.package,
                                           new_data_addr.die, new_data_addr.plane,
                                           new_data_addr.block, i));
                }
            }
        }

        if (exchange) {
            block_erases.insert(std::make_pair(new_data_block_id, 0));
            logic2data_map.insert(std::make_pair(new_data_block_id, data_block_id));
            std::map<size_t, size_t>::iterator it3;
            for (it3 = logic2data_map.begin();
                 it3 != logic2data_map.end(); it3++) {
                if (it3->first == new_data_block_id) continue;
                if (it3->second == data_block_id) {
                    it3->second = new_data_block_id;
                    break;
                }
            }
            if (logic2data_map.find(data_block_id) ==
                logic2data_map.end()) {
                logic2data_map.insert(std::make_pair(data_block_id, new_data_block_id));
            }
        }
        func(FlashSim::OpCode::ERASE, lba2addr(log_block->block_id *
                                               BLOCK_SIZE));
        log_block->isEmpty = true;
        increaseErase(data_block_id);
        increaseErase(log_block->block_id);
        resetLogBlock(log_block);
        data2log_map.erase(data_block_id);
        if (getEraseNum(log_block->block_id) == BLOCK_ERASES) {
            for (size_t j = (ADDRESSABLE / BLOCK_SIZE - 1); j >= 0; j--) {
                if (j == clean_ptr) continue;
                if (block_erases.find(j) == block_erases.end()) {
                    // The data block hasn't been written before.
                    std::cout << "Found an empty data block! Perform exchange! "
                            "log_block " << log_block->block_id
                              << " changed to: " << j << "\n";
                    changeLog2DataBlock(j, true, log_block);
                    break;
                }
                if (j==0) break;
            }
        }
    }

    // Perform cleaning when there are no empty log blocks.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyLogBlocks(
            const FlashSim::ExecCallBack<PageType> &func, size_t
    data_block_id, FlashSim::Address write_addr, size_t original_lba) {
        std::cout << "Enter noEmptyLogBlocks! data_block_id: "
                  << data_block_id
                  << ", clean: " << clean_ptr <<"\n";
        if (freed_log.size() > 0) {
            // If there are freed log blocks. Find the one with the largest
            // remaining life.
            typename std::set<LogBlock*>::iterator freed_log_it;
            freed_log_it = freed_log.begin();
            LogBlock* chosen_log = *freed_log_it;
            uint16_t min_erase = getEraseNum(chosen_log->block_id);
            freed_log_it++;
            while(freed_log_it != freed_log.end()) {
                if (getEraseNum((*freed_log_it)->block_id) <
                    min_erase) {
                    min_erase = getEraseNum((*freed_log_it)->block_id);
                    chosen_log = *freed_log_it;
                }
                freed_log_it++;
            }
            freed_log.erase(chosen_log);
            FlashSim::Address chosen_log_addr = lba2addr(chosen_log->block_id
                                                         * BLOCK_SIZE);
            if (!chosen_log->isEmpty) {
                func(FlashSim::OpCode::ERASE, chosen_log_addr);
                increaseErase(chosen_log->block_id);
            }
            chosen_log->isEmpty = false;
            data2log_map.insert(std::make_pair(data_block_id, chosen_log));
            std::cout << "Exit noEmptyLogBlocks! Found freed log! "
                      << " Map " << data_block_id << " -> "
                      << chosen_log->block_id << std::endl;
            if (data2log_map.size() > 31) {
                typename std::map<size_t, LogBlock* >::iterator tt;
                for (tt = data2log_map.begin(); tt != data2log_map.end(); tt++) {
                    std::cout << tt->first << "->"
                              << tt->second->block_id << std::endl;
                }
            }
            return chosen_log->write(write_addr, original_lba);
        }

        size_t clean_data_block_id = 0;
        size_t log_block_id = 0;
        uint16_t min_erase = BLOCK_ERASES;
        LogBlock *log_block = NULL;
        typename std::map<size_t, LogBlock *>::iterator it;
        for (it = data2log_map.begin(); it != data2log_map.end(); it++) {
            if (getEraseNum(it->second->block_id) < min_erase) {
                if (getEraseNum(it->first) >= BLOCK_ERASES) {
                    std::cout << "Found " << it->second->block_id
                              << ", but its data block " << it->first
                              << " can't erase!\n";
                    continue;
                }
                log_block = it->second;
                log_block_id = log_block->block_id;
                clean_data_block_id = it->first;
                min_erase = getEraseNum(log_block_id);
            }
        }

        if (log_block == NULL) {
            std::cout << "fail: cannot find suitable log block!"
                      << logic2data_map.size() << ", " << block_erases.size();
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        std::cout << "log chosen to be cleaned: ("
                  << clean_data_block_id << ", "
                  << log_block_id << ")\n";

        bool need_cleaning_block = !logHasAll(clean_data_block_id, log_block);

        if (need_cleaning_block) {
            bool move = false;
            if (clean_erases >= EXCHANGE
                && clean_ptr <= (RAW_CAPACITY / BLOCK_SIZE - CLEAN_NUM)) {
                move = changeCleaningBlock(func, clean_data_block_id);
                if (move) {
                    // If the clean_ptr changed.
                    if (clean_ptr == data_block_id) {
                        std::cout << "coincidence!\n";
                        DataBlockInfo new_data_block = getDataBlock
                                (original_lba);
                        data_block_id = new_data_block.id;
                        write_addr = new_data_block.addr;
                    }
                }
            }
            // Check if the cleaning block has reached its erase limit.
            if (clean_erases >= BLOCK_ERASES) {
                move = moveCleanPtr();
                if(!move) {
                    std::cout << "fail: cleaning page can't erase anymore! In "
                            "noEmptyLogBlocks!"
                              << logic2data_map.size() << ", " << block_erases.size();
                    return std::make_pair(FlashSim::ExecState::FAILURE,
                                          FlashSim::Address(0, 0, 0, 0, 0));
                }
            }
            if (move) {
                standardCleaning(clean_data_block_id, log_block, func, true,
                                 false);
            } else {
                standardCleaning(clean_data_block_id, log_block, func, true,
                                 true);
            }
        } else {
            standardCleaning(clean_data_block_id, log_block, func, false,
                             false);
        }
        data2log_map.insert(std::make_pair(data_block_id, log_block));
        log_block->isEmpty = false;
        return log_block->write(write_addr, original_lba);
    }

    // Perform cleaning when the log block has no empty pages.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyPages(
            const FlashSim::ExecCallBack<PageType> &func, size_t
    data_block_id, FlashSim::Address write_addr) {
        std::cout << "noEmptyPages cleaning! data_block_id: "
                  << data_block_id << ", erases: "
                  << getEraseNum(data_block_id)
                  << ", clean_ptr: "
                  << clean_ptr
                  << ", clean_erases: " << clean_erases << "\n";
        if (getEraseNum(data_block_id) >= BLOCK_ERASES) {
            std::cout << "fail: data block can't erase anymore! In noEmptyPages."
                      << logic2data_map.size() << ", " << block_erases.size();
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        size_t data_block_start = data_block_id * BLOCK_SIZE;
        FlashSim::Address data_addr = lba2addr(data_block_start);
        typename std::map<size_t, LogBlock *>::iterator it = data2log_map.find(data_block_id);
        LogBlock *log_block = it->second;
        FlashSim::Address cleaning_addr;
        size_t new_data_block_id = data_block_id;
        FlashSim::Address new_data_addr = data_addr;
        size_t min_erase = BLOCK_ERASES;
        bool found_empty_block = false;
        bool swap_data_block = false;
        if (getEraseNum(data_block_id) >= EXCHANGE) {
            for (size_t j = (ADDRESSABLE / BLOCK_SIZE - 1); j >= 0; j--) {
                if (j == clean_ptr) continue;
                if (block_erases.find(j) == block_erases.end()) {
                    // The data block hasn't been written before.
                    std::cout << "Found an empty data block! Perform exchange! "
                            "data_block " << data_block_id
                              << " changed to: " << j << "\n";
                    found_empty_block = true;
                    new_data_block_id = j;
                    new_data_addr = lba2addr(new_data_block_id * BLOCK_SIZE);
                    block_erases.insert(std::make_pair(j, 0));
                    break;
                } else {
                    if (getEraseNum(j) < min_erase) {
                        min_erase = getEraseNum(j);
                        new_data_block_id = j;
                        new_data_addr = lba2addr(new_data_block_id * BLOCK_SIZE);
                        swap_data_block = true;
                    }
                }
                if (j==0) break;
            }
        }

        if (found_empty_block) {
            swap_data_block = false;
        } else {
            if (swap_data_block) {
                std::cout << "Found " << new_data_block_id
                          << " with erase: "
                          << min_erase << ". Swap!\n";
            }
        }

        bool need_cleaning_block = !logHasAll(data_block_id, log_block);

        if (clean_ptr == RAW_CAPACITY / BLOCK_SIZE) {
            clean_ptr --;
            cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
        } else {
            cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
            if (need_cleaning_block) {
                bool move;
                if (clean_erases >= EXCHANGE
                    && clean_ptr <= (RAW_CAPACITY / BLOCK_SIZE) - CLEAN_NUM) {
                    move = changeCleaningBlock(func, data_block_id);
                }
                if (clean_erases >= BLOCK_ERASES) {
                    move = moveCleanPtr();
                    if (!move) {
                        std::cout << "fail: cleaning page can't erase anymore! In noEmptyPages."
                                  << logic2data_map.size() << ", " << block_erases.size();
                        return std::make_pair(FlashSim::ExecState::FAILURE,
                                              FlashSim::Address(0, 0, 0, 0, 0));
                    }
                }
                if (move) {
                    cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
                } else {
                    func(FlashSim::OpCode::ERASE, cleaning_addr);
                    clean_erases++;

                }
            }
        }

        if (need_cleaning_block) {
            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                // Only save the page that has been written before
                if (written.find(getLba(data_block_id, i)) != written.end()) {
                    if (garbage.find(getLba(data_block_id, i)) != garbage.end()) {
                        garbage.erase(getLba(data_block_id, i));
                        if (write_addr.page != i)
                            written.erase(getLba(data_block_id, i));
                        continue;
                    }
                    if (write_addr.page == i) {
                        continue;
                    }
                    std::map<uint16_t, uint16_t>::iterator it2 =
                            log_block->log_page_map.find(i);
                    if (it2 == log_block->log_page_map.end()) {
                        // No copy in the log block.
                        func(FlashSim::OpCode::READ,
                             FlashSim::Address(data_addr.package,
                                               data_addr.die, data_addr.plane,
                                               data_addr.block, i));
                    } else {
                        func(FlashSim::OpCode::READ,
                             FlashSim::Address(log_block->package,
                                               log_block->die, log_block->plane,
                                               log_block->block, it2->second));
                    }
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(cleaning_addr.package,
                                           cleaning_addr.die, cleaning_addr.plane,
                                           cleaning_addr.block, i));
                }
            }

            func(FlashSim::OpCode::ERASE, data_addr);

            if (swap_data_block) {
                // Copy live pages back to the data block
                for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                    if (written.find(getLba(new_data_block_id, i)) != written
                            .end()
                            ) {
                        if (garbage.find(getLba(new_data_block_id, i)) !=
                            garbage.end()) {
                            garbage.erase(getLba(new_data_block_id, i));
                            written.erase(getLba(new_data_block_id, i));
                            continue;
                        }
                        func(FlashSim::OpCode::READ,
                             FlashSim::Address(new_data_addr.package,
                                               new_data_addr.die, new_data_addr.plane,
                                               new_data_addr.block, i));
                        func(FlashSim::OpCode::WRITE,
                             FlashSim::Address(data_addr.package,
                                               data_addr.die, data_addr.plane,
                                               data_addr.block, i));
                    }
                }
                func(FlashSim::OpCode::ERASE, new_data_addr);
                block_erases.find(new_data_block_id)->second++;
            }

            // Copy live pages back to the data block
            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                if (written.find(getLba(data_block_id, i)) != written.end() &&
                    write_addr.page != i) {
                    func(FlashSim::OpCode::READ,
                         FlashSim::Address(cleaning_addr.package,
                                           cleaning_addr.die, cleaning_addr.plane,
                                           cleaning_addr.block, i));
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(new_data_addr.package,
                                           new_data_addr.die, new_data_addr.plane,
                                           new_data_addr.block, i));
                }
            }
        } else {
            func(FlashSim::OpCode::ERASE, data_addr);
            if (swap_data_block) {
                // Copy live pages back to the data block
                for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                    if (written.find(getLba(new_data_block_id, i)) != written
                            .end()
                            ) {
                        if (garbage.find(getLba(new_data_block_id, i)) !=
                            garbage.end()) {
                            garbage.erase(getLba(new_data_block_id, i));
                            written.erase(getLba(new_data_block_id, i));
                            continue;
                        }
                        func(FlashSim::OpCode::READ,
                             FlashSim::Address(new_data_addr.package,
                                               new_data_addr.die, new_data_addr.plane,
                                               new_data_addr.block, i));
                        func(FlashSim::OpCode::WRITE,
                             FlashSim::Address(data_addr.package,
                                               data_addr.die, data_addr.plane,
                                               data_addr.block, i));
                    }
                }
                func(FlashSim::OpCode::ERASE, new_data_addr);
                block_erases.find(new_data_block_id)->second++;
            }
            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                if (written.find(getLba(data_block_id, i)) != written.end()) {
                    if (garbage.find(getLba(data_block_id, i)) != garbage.end()) {
                        garbage.erase(getLba(data_block_id, i));
                        if (write_addr.page != i)
                            written.erase(getLba(data_block_id, i));
                        continue;
                    }
                    if (write_addr.page == i) {
                        continue;
                    }
                    std::map<uint16_t, uint16_t>::iterator it_page =
                            log_block->log_page_map.find(i);
                    func(FlashSim::OpCode::READ,
                         FlashSim::Address(log_block->package,
                                           log_block->die, log_block->plane,
                                           log_block->block, it_page->second));
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(new_data_addr.package,
                                           new_data_addr.die, new_data_addr.plane,
                                           new_data_addr.block, i));
                }
            }
        }
        increaseErase(data_block_id);
        resetLogBlock(log_block);

        if (getEraseNum(log_block->block_id) >= EXCHANGE) {
            for (size_t j = (ADDRESSABLE / BLOCK_SIZE - 1); j >= 0; j--) {
                if (j == clean_ptr) continue;
                if (block_erases.find(j) == block_erases.end()) {
                    // The data block hasn't been written before.
                    std::cout << "Found an empty data block! Perform found_empty_block! "
                            "log_block " << log_block->block_id
                              << " changed to: " << j << "\n";
                    changeLog2DataBlock(j, true, log_block);
                    break;
                }
                if (j == 0) break;
            }
        }
        if (getEraseNum(log_block->block_id) < BLOCK_ERASES) {
            freed_log.insert(log_block);
        }
        data2log_map.erase(data_block_id);
        if (found_empty_block) {
            block_erases.insert(std::make_pair(new_data_block_id, 0));
            logic2data_map.insert(std::make_pair(new_data_block_id, data_block_id));
            std::cout << "insert: ("
                      << new_data_block_id << ", "
                      << data_block_id << ")\n";
            std::map<size_t, size_t>::iterator it3;
            for (it3 = logic2data_map.begin();
                 it3 != logic2data_map.end(); it3++) {
                if (it3->first == new_data_block_id) continue;
                if (it3->second == data_block_id) {
                    it3->second = new_data_block_id;
                    std::cout << "update: ("
                              << it3->first << ", "
                              << data_block_id << ")->("
                              << it3->first << ", "
                              << new_data_block_id << ")\n";
                    break;
                }
            }
            if (logic2data_map.find(data_block_id) ==
                logic2data_map.end()) {
                logic2data_map.insert(std::make_pair(data_block_id, new_data_block_id));
                std::cout << "insert: ("
                          << data_block_id << ", "
                          << new_data_block_id << ")\n";
            }
            new_data_addr.page = write_addr.page;
            write_addr = new_data_addr;
        } else {
            if (swap_data_block) {
                std::map<size_t, size_t>::iterator it3;
                size_t old_logic_id = RAW_CAPACITY / BLOCK_SIZE;
                for (it3 = logic2data_map.begin();
                     it3 != logic2data_map.end(); it3++) {
                    if (it3->second == data_block_id) {
                        it3->second = new_data_block_id;
                        std::cout << "update: ("
                                  << it3->first << ", "
                                  << data_block_id << ")->("
                                  << it3->first << ", "
                                  << new_data_block_id << ")\n";
                        old_logic_id = it3->first;
                        break;
                    }
                }
                if (logic2data_map.find(data_block_id) ==
                    logic2data_map.end()) {
                    logic2data_map.insert(std::make_pair(data_block_id, new_data_block_id));
                }
                for (it3 = logic2data_map.begin();
                     it3 != logic2data_map.end(); it3++) {
                    if (it3->first == old_logic_id) continue;
                    if (it3->second == new_data_block_id) {
                        it3->second = data_block_id;
                        std::cout << "update: ("
                                  << it3->first << ", "
                                  << new_data_block_id << ")->("
                                  << it3->first << ", "
                                  << data_block_id << ")\n";
                        break;
                    }
                }
                if (logic2data_map.find(new_data_block_id) ==
                    logic2data_map.end()) {
                    logic2data_map.insert(std::make_pair(new_data_block_id,
                                                         data_block_id));
                    std::cout << "insert: ("
                              << new_data_block_id << ", "
                              << data_block_id << ")\n";
                }
                new_data_addr.page = write_addr.page;
                write_addr = new_data_addr;
            }
        }
        return std::make_pair(FlashSim::ExecState::SUCCESS, write_addr);
    }

    /*
     * Optionally mark a LBA as a garbage.
     */
    FlashSim::ExecState
    Trim(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        garbage.insert(lba);
        return FlashSim::ExecState::SUCCESS;
    }
};

/*
 * CreateMyFTL() - Creates class MyFTL object
 *
 * You do not need to modify this
 */
FlashSim::FTLBase<TEST_PAGE_TYPE> *FlashSim::CreateMyFTL(const Configuration *conf) {
    return new MyFTL<TEST_PAGE_TYPE>(conf);
}
