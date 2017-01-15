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
uint8_t CLEAN_NUM = 16;
//uint8_t same_cout = 0;
//size_t last_write = 0;
//size_t most_recent = 0;
//bool same = false;
bool clean_dirty;

std::set<size_t> written;
std::set<size_t> garbage;
std::set<size_t> failed;
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
                std::cout << "Found a more recent copy (page " << it->second << ") in logs. Return that copy\n";
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                                      FlashSim::Address(package, die, plane, block,
                                                        it->second));
            } else {
                std::cout << "No more recent copy in logs. Return the calculated PA.\n";
                return std::make_pair(FlashSim::ExecState::SUCCESS, addr);
            }
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> write(
                FlashSim::Address addr, size_t original_lba) {
            if (empty_page_start == BLOCK_SIZE) {
                std::cout << "The mapped block is full!\n";
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

    // Change the empty data block log.
    void changeLog2DataBlock(size_t data_block_id, LogBlock *
    log_block) {
        block_erases.insert(std::make_pair(data_block_id, 0));
        logic2data_map.insert(std::make_pair(data_block_id,
                                             log_block->block_id));
        std::cout << "Declared" << data_block_id << " failed."
                  << " Changed it to log!\n";
        log_block->isEmpty = true;
        failed.insert(data_block_id);
    }

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
        clean_ptr = RAW_CAPACITY / BLOCK_SIZE - 1;
        clean_dirty = false;
        EXCHANGE = BLOCK_ERASES - 1;
        std::cout << "\nSSD_SIZE: " << unsigned(SSD_SIZE) << "\nPACKAGE_SIZE: "
                  << unsigned(PACKAGE_SIZE) << "\nDIE_SIZE: " << DIE_SIZE
                  << "\nPLANE_SIZE: " << PLANE_SIZE << "\nBLOCK_SIZE: "
                  << BLOCK_SIZE << "\nOVERPROVISIONING: " << unsigned(OVERPROVISIONING)
                  << "\nRAW_CAPACITY: " << RAW_CAPACITY << "\nempty_log_block: "
                  << empty_log_block << "\nADRESSABLE: " << ADDRESSABLE
                  << "\nBLOCK_ERASES: "
                  << BLOCK_ERASES << std::endl;
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
        std::cout << "\nRead: " << lba << "\n";
//        if (same && lba == last_write) {
//            std::cout << "directly read: " << most_recent << "\n";
//            return std::make_pair(FlashSim::ExecState::SUCCESS,
//                                  lba2addr(most_recent));
//        }
        if (written.find(lba) == written.end()) {
            // The page has never been written before.
            std::cout << "booo: The page hasn't been written before\n";
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        if (lba >= ADDRESSABLE) {
            // Invalid lba
            std::cout << "booo: lba out of bound!\n";
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
            std::cout << "No block mapped to it. Return the calculated PA\n";
            return std::make_pair(FlashSim::ExecState::SUCCESS, data_block.addr);
        } else {
            // Check if there is a more recent copy in log-reservation blocks.
            LogBlock *logBlock = it->second;
            std::cout << "Found block " << it->first << " mapped to " << logBlock->block_id << "\n";
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
        std::cout << "\nWrite to: " << lba << "\n";
        std::cout << "block map size: " << data2log_map.size() << std::endl;
        // Invalid lba
        if (lba >= ADDRESSABLE) {
            std::cout << "booo: lba out of bound!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
//        if (lba == last_write) {
//            if (!same) {
//                same_cout++;
//                std::cout << unsigned(same_cout) << "\n";
//            }
//            if (same || same_cout >= (BLOCK_SIZE + 1)) {
//                same = true;
//                return sameWrite(func);
//            }
//        } else {
//            same_cout = 0;
//            last_write = lba;
//        }
        DataBlockInfo data_block = getDataBlock(lba);
        if (written.find(lba) != written.end()) {
            // The page is not empttypename std::map<size_t, LogBlock *>::iterator it = data2log_map
            .find(data_block.id);y

            if (failed.find(data_block.id) != failed.end()) {
                std::cout << "The data block has been declared failed!\n";
                return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
            }
            if (it == data2log_map.end()) { // No log block mapped to it.
                if (freed_log.size() > 0) {
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
                    if (!chosen_log->isEmpty) {
                        func(FlashSim::OpCode::ERASE, lba2addr
                                (chosen_log->block_id * BLOCK_SIZE));
                        block_erases.find(chosen_log->block_id)->second++;
                    }
                    data2log_map.insert(std::make_pair(data_block.id,
                                                       chosen_log));
                    chosen_log->isEmpty = false;
                    return chosen_log->write(data_block.addr, lba);
                }
                if (empty_log_block >= (RAW_CAPACITY - BLOCK_SIZE * CLEAN_NUM)) {
                    // Log-reservation blocks are all mapped.
                    std::cout << "Log-reservation blocks are full! Need cleaning\n";
                    return noEmptyLogBlocks(func, data_block.id, data_block
                            .addr, lba);
                } else {
                    // Map a new log-reservation block to the data block.
                    std::cout << "Map a new log-reservation block to the data block: "
                              << data_block.id << "->" << empty_log_block /
                            BLOCK_SIZE << std::endl;
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
                std::cout << "Found " << it->first << " mapped to " << logBlock->block_id << "\n";
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

    // When writing to the same address.
//    std::pair<FlashSim::ExecState, FlashSim::Address>
//    sameWrite(const FlashSim::ExecCallBack<PageType> &func) {
//        if (most_recent < ADDRESSABLE - 1) {
//            if ((most_recent + 1) % BLOCK_SIZE == 0) {
//                std::cout << "last page!\n";
//                if (clean_erases < BLOCK_ERASES) {
//                    FlashSim::Address cleaning_addr = lba2addr(most_recent);
//                    std::cout << "Address to be cleaned: "
//                              << unsigned(cleaning_addr.package) << ", "
//                              << unsigned(cleaning_addr.die) << ", "
//                              << cleaning_addr.plane << ", "
//                              << cleaning_addr.block << ", "
//                              << cleaning_addr.page << "\n";
//                    cleaning_addr.page = 0;
//                    func(FlashSim::OpCode::ERASE, cleaning_addr);
//                    clean_erases++;
//                    most_recent = most_recent + 1 - BLOCK_SIZE;
//                    std::cout << "direct write to " << most_recent << "\n";
//                    return std::make_pair(FlashSim::ExecState::SUCCESS,
//                                          lba2addr(most_recent));
//                } else {
//                    most_recent++;
//                    clean_erases = 0;
//                    std::cout << "Move to block: " << most_recent /
//                                                      BLOCK_SIZE << "\n";
//                    std::cout << "direct write to " << most_recent << "\n";
//                    return std::make_pair(FlashSim::ExecState::SUCCESS,
//                                          lba2addr(most_recent));
//                }
//            } else {
//                most_recent++;
//                clean_erases = 0;
//                std::cout << "direct write to " << most_recent << "\n";
//                return std::make_pair(FlashSim::ExecState::SUCCESS,
//                                      lba2addr(most_recent));
//            }
//        }
//        std::cout << "fail: stop\n";
//        return std::make_pair(FlashSim::ExecState::FAILURE,
//                              FlashSim::Address(0, 0, 0, 0, 0));
//    }

    // Move cleaning pointer to the next empty one if possible
    bool findNewClean() {
        if (clean_ptr >= RAW_CAPACITY / BLOCK_SIZE
                || clean_ptr <= (RAW_CAPACITY / BLOCK_SIZE - CLEAN_NUM)) {
            return false;
        } else {
            std::cout << "Move clean_ptr " << clean_ptr
                      << "->" << clean_ptr - 1 << "\n";
            clean_ptr --;
            clean_erases = 0;
            clean_dirty = false;
            return true;
        }
    }

    // Try to change a log/clean block to data block.
    size_t findNewDataBlock(const
                            FlashSim::ExecCallBack<PageType> &func,
                            size_t data_block_id, bool need_clean_block) {
        std::cout << "Enter findNewDataBlock!\n";
        size_t new_data_block_id = data_block_id;
        if (freed_log.size() > 0) {
            typename std::set<LogBlock *>::iterator freed_log_it;
            freed_log_it = freed_log.begin();
            LogBlock *chosen_log = *freed_log_it;
            uint16_t min_erase = getEraseNum(chosen_log->block_id);
            freed_log_it++;
            while (freed_log_it != freed_log.end()) {
                if (getEraseNum((*freed_log_it)->block_id) <
                    min_erase) {
                    min_erase = getEraseNum((*freed_log_it)->block_id);
                    chosen_log = *freed_log_it;
                }
                freed_log_it++;
            }
            freed_log.erase(chosen_log);
            std::cout << "changed" << chosen_log->block_id
                      << " to data block!\n";
            new_data_block_id = chosen_log->block_id;
            FlashSim::Address log_addr = lba2addr(clean_ptr *
                                                    BLOCK_SIZE);
            func(FlashSim::OpCode::ERASE, log_addr);
            increaseErase(new_data_block_id);
            delete chosen_log;
            if (logic2data_map.find(data_block_id) == logic2data_map.end()) {
                logic2data_map.insert(std::make_pair(data_block_id, new_data_block_id));
            } else {
                std::map<size_t, size_t>::iterator it3;
                for (it3 = logic2data_map.begin(); it3 != logic2data_map.end
                        (); it3++) {
                    if (it3->second == data_block_id) {
                        it3->second = new_data_block_id;
                    }
                }
            }
            return new_data_block_id;
        }
        if (clean_ptr >= (RAW_CAPACITY / BLOCK_SIZE)) {
            // No cleaning block available.
            return new_data_block_id;
        }
        if (clean_ptr <= (RAW_CAPACITY / BLOCK_SIZE - CLEAN_NUM)) {
            // No spared cleaning block.
            if (need_clean_block) {
                return new_data_block_id;
            } else {
                if (clean_erases < BLOCK_ERASES) {
                    if (clean_dirty) {
                        FlashSim::Address clean_addr = lba2addr(clean_ptr *
                                                                BLOCK_SIZE);
                        func(FlashSim::OpCode::ERASE, clean_addr);
                        clean_erases++;
                        clean_dirty = false;
                    }
                    std::cout << "changed clean_ptr " << clean_ptr
                              << " to data block!\n";
                    new_data_block_id = clean_ptr;
                    clean_ptr = RAW_CAPACITY / BLOCK_SIZE;
                    if (logic2data_map.find(data_block_id) == logic2data_map.end()) {
                        logic2data_map.insert(std::make_pair(data_block_id, clean_ptr));
                    } else {
                        std::map<size_t, size_t>::iterator it3;
                        for (it3 = logic2data_map.begin(); it3 != logic2data_map.end
                                (); it3++) {
                            if (it3->second == data_block_id) {
                                it3->second = new_data_block_id;
                            }
                        }
                    }
                    block_erases.insert(std::make_pair(new_data_block_id,
                                                       clean_erases));
                }
            }
        } else {
            if (clean_dirty) {
                FlashSim::Address clean_addr = lba2addr(clean_ptr *
                                                        BLOCK_SIZE);
                func(FlashSim::OpCode::ERASE, clean_addr);
                clean_erases++;
                clean_dirty = false;
            }
            new_data_block_id = clean_ptr;
            std::cout << "changed clean_ptr " << clean_ptr
                      << " to data block!\n";
            if (logic2data_map.find(data_block_id) == logic2data_map.end()) {
                logic2data_map.insert(std::make_pair(data_block_id, new_data_block_id));
            } else {
                std::map<size_t, size_t>::iterator it3;
                for (it3 = logic2data_map.begin(); it3 != logic2data_map.end
                        (); it3++) {
                    if (it3->second == data_block_id) {
                        it3->second = new_data_block_id;
                    }
                }
            }
            clean_ptr--;
            clean_dirty = false;
            block_erases.insert(std::make_pair(new_data_block_id,
                                               clean_erases));
            clean_erases = 0;
        }
        return new_data_block_id;
    }


        // Perform cleaning given the data block and log block and whether a
    // cleaning block is needed.
    void standardCleaning(size_t data_block_id, LogBlock* log_block, const
    FlashSim::ExecCallBack<PageType> &func, bool need_cleaning_block, bool
    erase_clean) {
        FlashSim::Address data_addr = lba2addr(data_block_id * BLOCK_SIZE);
        FlashSim::Address cleaning_addr;
        size_t new_data_block_id = data_block_id;
        FlashSim::Address new_data_addr = data_addr;
        std::cout << "Enter standardCleaning: data_block_id "
                  << data_block_id
                  << ", log_block_id " << log_block->block_id
                  << ", need clean_block? " << need_cleaning_block
                  << ", clean_ptr " << clean_ptr
                  << ", clean_erases: " << clean_erases
                  << ", data2log size: " << data2log_map.size()
                  << ", logic2data size: " << logic2data_map.size()
                  << std::endl;

        cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
        if (need_cleaning_block && clean_dirty) {
            func(FlashSim::OpCode::ERASE, cleaning_addr);
            clean_erases++;
        }

//        bool exchange = false;
//        if (getEraseNum(data_block_id) >= EXCHANGE) {
//            for (size_t j = (ADDRESSABLE / BLOCK_SIZE - 1); j >= 0; j--) {
//                if (j == clean_ptr) continue;
//                if (block_erases.find(j) == block_erases.end()) {
//                    // The data block hasn't been written before.
//                    std::cout << "Found an empty data block! Perform exchange! "
//                            "data_block " << data_block_id
//                              << " changed to: " << j << "\n";
//                    exchange = true;
//                    new_data_block_id = j;
//                    new_data_addr = lba2addr(new_data_block_id * BLOCK_SIZE);
//                    block_erases.insert(std::make_pair(j, 0));
//                    break;
//                }
//                if (j==0) break;
//            }
//        }

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
            clean_dirty = true;
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

//        if (exchange) {
//            block_erases.insert(std::make_pair(new_data_block_id, 0));
//            logic2data_map.insert(std::make_pair(new_data_block_id, data_block_id));
//            std::map<size_t, size_t>::iterator it3;
//            for (it3 = logic2data_map.begin();
//                 it3 != logic2data_map.end(); it3++) {
//                if (it3->first == new_data_block_id) continue;
//                if (it3->second == data_block_id) {
//                    it3->second = new_data_block_id;
//                    break;
//                }
//            }
//            if (logic2data_map.find(data_block_id) ==
//                logic2data_map.end()) {
//                logic2data_map.insert(std::make_pair(data_block_id, new_data_block_id));
//            }
//        }
        func(FlashSim::OpCode::ERASE, lba2addr(log_block->block_id *
                                               BLOCK_SIZE));
        log_block->isEmpty = true;
        increaseErase(data_block_id);
        increaseErase(log_block->block_id);
        resetLogBlock(log_block);
        data2log_map.erase(data_block_id);
    }

    // Perform cleaning when there are no empty log blocks.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyLogBlocks(
            const FlashSim::ExecCallBack<PageType> &func, size_t
    data_block_id, FlashSim::Address write_addr, size_t original_lba) {
        std::cout << "Enter noEmptyLogBlocks! data_block_id: "
                  << data_block_id
                  << ", clean: " << clean_ptr <<"\n";
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
            for (size_t j = (ADDRESSABLE / BLOCK_SIZE - 1); j >= 0; j--) {
                if (j == clean_ptr) continue;
                std::cout << getEraseNum(j) << " ";
                if (block_erases.find(j) == block_erases.end()) {
                    // The data block hasn't been written before.
                    FlashSim::Address new_log_block_addr
                            = lba2addr(j * BLOCK_SIZE);
                    log_block = new LogBlock(&new_log_block_addr, j);
                    std::cout
                            << "Found an empty data block! "
                            << j << "\n";
                    changeLog2DataBlock(j, log_block);
                    data2log_map.insert(std::make_pair(data_block_id, log_block));
                    return log_block->write(write_addr, original_lba);
                    break;
                }
                if (j == 0) break;
            }
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
            // Check if the cleaning block has reached its erase limit.
            if (clean_erases >= BLOCK_ERASES) {
                move = findNewClean();
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
        FlashSim::Address data_addr = lba2addr(data_block_id * BLOCK_SIZE);
        typename std::map<size_t, LogBlock *>::iterator it = data2log_map.find(data_block_id);
        LogBlock *log_block = it->second;
        FlashSim::Address cleaning_addr;
        size_t new_data_block_id = data_block_id;
        FlashSim::Address new_data_addr = data_addr;
        size_t min_erase = BLOCK_ERASES;
        bool found_empty_block = false;
        bool found_new_block = false;
        bool swap_data_block = false;
        if (getEraseNum(data_block_id) >= BLOCK_ERASES) {
            for (size_t j = (ADDRESSABLE / BLOCK_SIZE - 1); j >= 0; j--) {
                if (j == clean_ptr) continue;
                std::cout << getEraseNum(j);
                if (block_erases.find(j) == block_erases.end()) {
                    // The data block hasn't been written before.
                    std::cout << "Found an empty data block! Perform exchange! "
                            "data_block " << data_block_id
                              << " changed to: " << j << "\n";
                    found_empty_block = true;
                    found_new_block = true;
                    new_data_block_id = j;
                    new_data_addr = lba2addr(new_data_block_id * BLOCK_SIZE);
                    block_erases.insert(std::make_pair(j, 0));
                    break;
                }
                if (j==0) break;
            }
        }

//        if (found_empty_block) {
//            swap_data_block = false;
//        } else {
//            if (swap_data_block) {
//                std::cout << "Found " << new_data_block_id
//                          << " with erase: "
//                          << min_erase << ". Swap!\n";
//            }
//        }

        bool need_cleaning_block = !logHasAll(data_block_id, log_block);

        if (getEraseNum(data_block_id) >= BLOCK_ERASES && !found_empty_block) {
            new_data_block_id = findNewDataBlock(func, data_block_id,
                                                 need_cleaning_block);
            if (new_data_block_id == data_block_id) {
                std::cout << "fail: data block can't erase anymore!"
                          << " In noEmptyPages. "
                          << logic2data_map.size() << ", " << block_erases.size();
                return std::make_pair(FlashSim::ExecState::FAILURE,
                                      FlashSim::Address(0, 0, 0, 0, 0));
            }
            found_new_block = true;
        }

        cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
        if (need_cleaning_block) {
            bool move = false;
            if (clean_erases >= BLOCK_ERASES
                || clean_erases >= RAW_CAPACITY / BLOCK_SIZE) {
                move = findNewClean();
                if (!move) {
                    std::cout
                            << "fail: No clean blocks available! In "
                                    "noEmptyPages."
                            << logic2data_map.size() << ", "
                            << block_erases.size();
                    return std::make_pair(FlashSim::ExecState::FAILURE,
                                          FlashSim::Address(0, 0, 0, 0, 0));
                }
            }
            if (move) {
                cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
            } else {
                if (clean_dirty) {
                    func(FlashSim::OpCode::ERASE, cleaning_addr);
                    clean_erases++;
                }
            }
            clean_dirty = false;
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

            if (getEraseNum(data_block_id) < BLOCK_ERASES && !
                    found_new_block) {
                func(FlashSim::OpCode::ERASE, data_addr);
            }

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
            clean_dirty = true;
        } else {
            if (getEraseNum(data_block_id) < BLOCK_ERASES
                && !found_new_block) {
                func(FlashSim::OpCode::ERASE, data_addr);
            }
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
        resetLogBlock(log_block);
        if (!found_new_block) {
            increaseErase(data_block_id);
        }

//        if (getEraseNum(log_block->block_id) >= BLOCK_ERASES) {
//            if (empty_log_block < (RAW_CAPACITY - CLEAN_NUM * BLOCK_SIZE)) {
//                std::cout << "Move log block "
//                          << log_block->block_id
//                          << " to the next empty one!\n";
//                delete log_block;
//                FlashSim::Address new_log_block = lba2addr(empty_log_block);
//                log_block = new LogBlock(&new_log_block, empty_log_block /
//                                                         BLOCK_SIZE);
//                empty_log_block += BLOCK_SIZE;
//                block_erases.insert(std::make_pair(log_block->block_id, 0));
//                log_block->isEmpty = true;
//                goto out_of_if;
//            }
//            for (size_t j = (ADDRESSABLE / BLOCK_SIZE - 1); j >= 0; j--) {
//                if (j == clean_ptr) continue;
//                std::cout << getEraseNum(j) << " ";
//                if (block_erases.find(j) == block_erases.end()) {
//                    // The data block hasn't been written before.
//                    std::cout << "Found an empty data block! Perform found_empty_block! "
//                            "log_block " << log_block->block_id
//                              << " changed to: " << j << "\n";
//                    changeLog2DataBlock(j, true, log_block);
//                    break;
//                }
//                if (j == 0) break;
//            }
//        }
//        out_of_if:
        if (getEraseNum(log_block->block_id) < BLOCK_ERASES) {
            freed_log.insert(log_block);
            log_block->isEmpty = false;
        }
        data2log_map.erase(data_block_id);
        if (found_empty_block) {
            block_erases.insert(std::make_pair(new_data_block_id, 0));
            logic2data_map.insert(std::make_pair(new_data_block_id, data_block_id));
            std::cout << "insert: ("
                      << new_data_block_id << ", "
                      << data_block_id << "). The second is declared "
                    "failed\n";
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
            failed.insert(data_block_id);
        } else if (found_new_block) {
            if (logic2data_map.find(data_block_id) == logic2data_map.end()) {
                logic2data_map.insert(std::make_pair(data_block_id,
                                                     new_data_block_id));
                std::cout << "insert: ("
                          << new_data_block_id << ", "
                          << data_block_id << "). The second is declared "
                                  "failed\n";
            } else {
                std::map<size_t, size_t>::iterator it3;
                for (it3 = logic2data_map.begin();
                     it3 != logic2data_map.end(); it3++) {
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
            }
//            if (swap_data_block) {
//                std::map<size_t, size_t>::iterator it3;
//                size_t old_logic_id = RAW_CAPACITY / BLOCK_SIZE;
//                for (it3 = logic2data_map.begin();
//                     it3 != logic2data_map.end(); it3++) {
//                    if (it3->second == data_block_id) {
//                        it3->second = new_data_block_id;
//                        std::cout << "update: ("
//                                  << it3->first << ", "
//                                  << data_block_id << ")->("
//                                  << it3->first << ", "
//                                  << new_data_block_id << ")\n";
//                        old_logic_id = it3->first;
//                        break;
//                    }
//                }
//                if (logic2data_map.find(data_block_id) ==
//                    logic2data_map.end()) {
//                    logic2data_map.insert(std::make_pair(data_block_id, new_data_block_id));
//                }
//                for (it3 = logic2data_map.begin();
//                     it3 != logic2data_map.end(); it3++) {
//                    if (it3->first == old_logic_id) continue;
//                    if (it3->second == new_data_block_id) {
//                        it3->second = data_block_id;
//                        std::cout << "update: ("
//                                  << it3->first << ", "
//                                  << new_data_block_id << ")->("
//                                  << it3->first << ", "
//                                  << data_block_id << ")\n";
//                        break;
//                    }
//                }
//                if (logic2data_map.find(new_data_block_id) ==
//                    logic2data_map.end()) {
//                    logic2data_map.insert(std::make_pair(new_data_block_id,
//                                                         data_block_id));
//                    std::cout << "insert: ("
//                              << new_data_block_id << ", "
//                              << data_block_id << ")\n";
//                }
//                new_data_addr.page = write_addr.page;
//                write_addr = new_data_addr;
//            }
        }
        return std::make_pair(FlashSim::ExecState::SUCCESS, write_addr);
    }

    /*
     * Optionally mark a LBA as a garbage.
     */
    FlashSim::ExecState
    Trim(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        std::cout << "TRIM request: " << lba << "!";
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
