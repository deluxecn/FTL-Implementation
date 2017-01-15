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

std::set<size_t> written;
std::set<size_t> garbage;

std::map<size_t, size_t> logic2data_map;
std::map<size_t, uint16_t> block_erases; // <log_block_id, erases_num>

template<typename PageType>
class MyFTL : public FlashSim::FTLBase<PageType> {

public:

    class LogBlock;
    std::map<size_t, LogBlock*> data2log_map; // <data_block_id, LogBlock *>
    std::set<LogBlock*> freed_log;

    class LogBlock {
    public:
        uint8_t package;
        uint8_t die;
        uint16_t plane;
        uint16_t block;
        size_t block_id;
        uint16_t empty_page_start;
        std::map<uint16_t, uint16_t> log_page_map;

        LogBlock(FlashSim::Address *addr, size_t block_id) {
            package = addr->package;
            die = addr->die;
            plane = addr->plane;
            block = addr->block;
            this->block_id = block_id;
            empty_page_start = 0;
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> read(
                FlashSim::Address addr) {
            std::map<uint16_t, uint16_t>::iterator it = log_page_map.find
                    (addr.page);
            if (it != log_page_map.end()) {
//                std::cout << "Found a more recent copy (page " << it->second << ") in logs. Return that copy\n";
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                                      FlashSim::Address(package, die, plane, block,
                                                        it->second));
            } else {
//                std::cout << "No more recent copy in logs. Return the calculated PA.\n";
                return std::make_pair(FlashSim::ExecState::SUCCESS, addr);
            }
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> write(
                FlashSim::Address addr, size_t original_lba) {
            if (empty_page_start == BLOCK_SIZE) {
//                std::cout << "The mapped block is full!\n";
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
//                std::cout << "Update the new page id in map: (" << addr.page
//                          << ", " << log_page_map[addr.page] << ")->(" << addr.page
//                          << ", " << empty_page_start << ").\n";
                it->second = empty_page_start;
            } else {
//                std::cout << "Add new page pair (" << addr.page << ", "
//                          << empty_page_start << ") to the map.\n";
                log_page_map.insert(std::make_pair(addr.page, empty_page_start));
            }
            return std::make_pair(FlashSim::ExecState::SUCCESS,
                                  FlashSim::Address(package, die, plane, block,
                                                    empty_page_start++));
        }
    };

    class DataBlockInfo {
    public:
        FlashSim::Address addr;
        size_t id;
        DataBlockInfo(size_t id, FlashSim::Address addr) {
            this->addr = addr;
            this->id = id;
        }
    };

    // Return the data block id given lba.
    DataBlockInfo getDataBlock(size_t lba) {
        FlashSim::Address original_addr = lba2addr(lba);
        if (logic2data_map.find(lba / BLOCK_SIZE) == logic2data_map.end()) {
            return DataBlockInfo(lba / BLOCK_SIZE, original_addr);
        } else {
            size_t data_block_id = logic2data_map.find(lba / BLOCK_SIZE)->second;
            FlashSim::Address new_addr = lba2addr(data_block_id);
            new_addr.page = original_addr.page;
            return DataBlockInfo(data_block_id, new_addr);
        }
    }

    // Return the erase times given the block id.
    uint16_t getEraseNum(size_t block_id) {
        std::map<size_t, uint16_t>::iterator it = block_erases.find(block_id);
        if (it != block_erases.end()) {
            return it->second;
        } else {
            return 0;
        }

    }

    // Increase erase number by one.
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

    // Find empty data block when one data block reaches its erase limit.
//    size_t findEmptyDataBlock(size_t original_data_block) {
//        for (size_t i = 0; i < ADDRESSABLE / BLOCK_SIZE; i++) {
//            if (block_erases.find(i) == block_erases.end()) {
//                // The data block has not been used before.
//                logic2data_map.insert(std::make_pair(original_data_block,
//                                                     i));
//                if (data2log_map.find(original_data_block) != data2log_map
//                                                                      .end()) {
//                    LogBlock* log_block = data2log_map.find(original_data_block)
//                            ->second;
//                    data2log_map.insert(std::make_pair(i, log_block));
//                    data2log_map.erase(original_data_block);
//                }
//                return i;
//            }
//        }
//        return ADDRESSABLE / BLOCK_SIZE;
//    }

    // Relocate log block when it reaches erase limit.
//    LogBlock* relocateLog(size_t data_block_id, const
//    FlashSim::ExecCallBack<PageType> &func) {
//        if (empty_log_block < RAW_CAPACITY - BLOCK_SIZE) {
//            // Map a new log-reservation block to the data block.
////            std::cout << "Map a new log-reservation block to the data block: "
////                      << data_block_id << "->" << empty_log_block / BLOCK_SIZE
////                      << std::endl;
//            FlashSim::Address new_log_block = lba2addr(empty_log_block);
//            LogBlock *logBlock = new LogBlock(&new_log_block,
//                                              empty_log_block / BLOCK_SIZE);
//            empty_log_block += BLOCK_SIZE;
//            data2log_map.erase(data_block_id);
//            data2log_map.insert(std::make_pair(data_block_id, logBlock));
//            return logBlock;
//        } else if (.size() > 0) {
//            LogBlock* log_block = *.begin();
//            .erase(log_block);
//            func(FlashSim::OpCode::ERASE, lba2addr(log_block->block_id * BLOCK_SIZE));
//            return log_block;
//        } else {
//            return NULL;
//        }
//    }

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
       std::cout << "\nSSD_SIZE: " << unsigned(SSD_SIZE) << "\nPACKAGE_SIZE: "
                 << unsigned(PACKAGE_SIZE) << "\nDIE_SIZE: " << DIE_SIZE
                 << "\nPLANE_SIZE: " << PLANE_SIZE << "\nBLOCK_SIZE: "
                 << BLOCK_SIZE << "\nOVERPROVISIONING: " << unsigned(OVERPROVISIONING)
                 << "\nRAW_CAPACITY: " << RAW_CAPACITY << "\nempty_log_block: "
                 << empty_log_block << "\nBLOCK_ERASES: "
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
//        std::cout << "\nRead: " << lba << "\n";
        if (written.find(lba) == written.end()) {
            // The page has never been written before.
//            std::cout << "loooow: The page hasn't been written before\n";
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        if (lba >= ADDRESSABLE) {
            // Invalid lba
//            std::cout << "loooow: lba out of bound!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        if (garbage.find(lba) != garbage.end()) {
            // The page has already been garbage.
//            std::cout << "loooow: The page has already been garbage!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        DataBlockInfo data_block = getDataBlock(lba);
        typename std::map<size_t, LogBlock *>::iterator it = data2log_map
                .find(data_block.id);
        if (it == data2log_map.end()) {
            // No log-reservation block mapped to this data block.
//            std::cout << "No block mapped to it. Return the calculated PA\n";
            return std::make_pair(FlashSim::ExecState::SUCCESS, data_block.addr);
        } else {
            // Check if there is a more recent copy in log-reservation blocks.
            LogBlock *logBlock = it->second;
//            std::cout << "Found block " << it->first << " mapped to " << logBlock->block_id << "\n";
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
//        std::cout << "\nWrite to: " << lba << "\n";
//        std::cout << "block map size: " << data2log_map.size() << std::endl;
        // Invalid lba
        if (lba >= ADDRESSABLE) {
//            std::cout << "loooow: lba out of bound!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
        DataBlockInfo data_block = getDataBlock(lba);
        if (written.find(lba) != written.end()) {
            // The page is not empty
            typename std::map<size_t, LogBlock *>::iterator it = data2log_map
                    .find(data_block.id);
            if (it == data2log_map.end()) { // No log block mapped to it.
                if (empty_log_block >= (RAW_CAPACITY - BLOCK_SIZE)) {
                    // Log-reservation blocks are all mapped.
//                    std::cout << "Log-reservation blocks are full! Need cleaning\n";
                    return noEmptyLogBlocks(func, data_block.id, data_block
                            .addr, lba);
                } else {
                    // Map a new log-reservation block to the data block.
//                    std::cout << "Map a new log-reservation block to the data block: "
//                              << data_block.id << "->" << empty_log_block /
//                            BLOCK_SIZE << std::endl;
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
//                std::cout << "Found " << it->first << " mapped to " << logBlock->block_id << "\n";
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

    // Exchange cleaning block with the data block who has the largest
    // remaining life. Return true if that data block can be found.
    bool exchangeCleaningBlock(const
                               FlashSim::ExecCallBack<PageType> &func) {
        if (clean_erases == BLOCK_ERASES) return false;
        size_t data_block_id;
        uint16_t min_erase = BLOCK_ERASES;
        for (size_t i = 0; i< ADDRESSABLE / BLOCK_SIZE; i++) {
            if (block_erases.find(i) == block_erases.end()) {
                // The data block hasn't been written before.
                logic2data_map.insert(std::make_pair(data_block_id, clean_ptr));
                clean_ptr = data_block_id;
                clean_erases = 0;
                std::cout << "Found a empty data block! Performed exchange!\n";
                return true;
            }
//            std::cout << getEraseNum(i) << " ";
            if (getEraseNum(i) < min_erase) {
                min_erase = getEraseNum(i);
                data_block_id = i;
            }
        }
        if (min_erase == BLOCK_ERASES) {
            // Cannot find a suitable data block.
            std::cout << "Cannot find a suitable data block.\n";
            return false;
        }
//        size_t data_block_start = data_block_id * BLOCK_SIZE;
//        FlashSim::Address data_block_start_addr = lba2addr(data_block_start);
//        FlashSim::Address old_cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
//        if (data2log_map.find(data_block_id) == data2log_map.end()) {
//            // No log block mapped to this data block.
//            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
//                if (written.find(data_block_start + i) != written.end()) {
//                    if (garbage.find(data_block_start + i) != garbage.end()) {
//                        garbage.erase(data_block_start + i);
//                        written.erase(data_block_start + i);
//                        continue;
//                    }
//                    func(FlashSim::OpCode::READ,
//                         FlashSim::Address(data_block_start_addr.package,
//                                           data_block_start_addr.die,
//                                           data_block_start_addr.plane,
//                                           i));
//                    func(FlashSim::OpCode::WRITE,
//                         FlashSim::Address(old_cleaning_addr.package,
//                                           old_cleaning_addr.die,
//                                           old_cleaning_addr.plane,
//                                           i));
//                }
//            }
//
//        } else {
//
//        }
        return true;

    }

    // Perform cleaning given the data block and log block and whether a
    // cleaning block is needed.
    void logsFullCleaning(size_t data_block_id, LogBlock* log_block, const
    FlashSim::ExecCallBack<PageType> &func, bool need_cleaning_block) {
        size_t data_block_start = data_block_id * BLOCK_SIZE;
        FlashSim::Address data_addr = lba2addr(data_block_start);
        FlashSim::Address cleaning_addr;
//        std::cout << "perform logsFullCleaning: data_block_id " <<
//                  data_block_id
//                  << ", log_block_id " << log_block->block_id << " " <<
//                  need_cleaning_block << " clean_ptr " << clean_ptr <<
//                                                                    std::endl;
        if (clean_ptr == RAW_CAPACITY / BLOCK_SIZE) {
//            std::cout << "logsFullCleaning 000\n";
            clean_ptr--;
            cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);

        } else {
//            std::cout << "logsFullCleaning 000else\n";
            cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
            if (need_cleaning_block) {
                func(FlashSim::OpCode::ERASE, cleaning_addr);
                clean_erases++;
            }
        }

//        std::cout << "logsFullCleaning 111\n";

        if (need_cleaning_block) {
            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                // Only save the page that has been written before
                if (written.find(data_block_start + i) != written.end()) {
                    if (garbage.find(data_block_start + i) != garbage.end()) {
                        garbage.erase(data_block_start + i);
                        written.erase(data_block_start + i);
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
                if (written.find(data_block_start + i) != written.end()) {
                    func(FlashSim::OpCode::READ,
                         FlashSim::Address(cleaning_addr.package,
                                           cleaning_addr.die, cleaning_addr.plane,
                                           cleaning_addr.block, i));
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(data_addr.package,
                                           data_addr.die, data_addr.plane,
                                           data_addr.block, i));
                }
            }
//            std::cout << "logsFullCleaning 222\n";
        } else {
            func(FlashSim::OpCode::ERASE, data_addr);
            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                // Only save the page that has been written before
                if (written.find(data_block_start + i) != written.end()) {
                    if (garbage.find(data_block_start + i) != garbage.end()) {
                        garbage.erase(data_block_start + i);
                        written.erase(data_block_start + i);
                        continue;
                    }
                    std::map<uint16_t, uint16_t>::iterator it2 =
                            log_block->log_page_map.find(i);
                    func(FlashSim::OpCode::READ,
                         FlashSim::Address(log_block->package,
                                           log_block->die, log_block->plane,
                                           log_block->block, it2->second));
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(data_addr.package,
                                           data_addr.die, data_addr.plane,
                                           data_addr.block, i));
                }
            }
        }
        func(FlashSim::OpCode::ERASE, lba2addr(log_block->block_id *
                                               BLOCK_SIZE));
        increaseErase(data_block_id);
        increaseErase(log_block->block_id);
        resetLogBlock(log_block);
//        std::cout << "logsFullCleaning 333\n";
    }

    // Perform cleaning when there are no empty log blocks.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyLogBlocks(
            const FlashSim::ExecCallBack<PageType> &func, size_t
    data_block_id, FlashSim::Address wirte_addr, size_t original_lba) {
        if (freed_log.size() > 0) {
            // If there are freed log blocks. Find the one with the largest
            // remaining life.
            typename std::set<LogBlock*>::iterator freed_log_it;
            freed_log_it = freed_log.begin();
            LogBlock* chosen_log = *freed_log_it;
            uint16_t min_erase = block_erases.find(chosen_log->block_id)->second;
            freed_log_it++;
            while(freed_log_it != freed_log.end()) {
                if (block_erases.find((*freed_log_it)->block_id)->second <
                        min_erase) {
                    min_erase = block_erases.find((*freed_log_it)->block_id)
                            ->second;
                    chosen_log = *freed_log_it;
                }
                freed_log_it++;
            }
            FlashSim::Address chosen_log_addr = lba2addr(chosen_log->block_id
                                                         * BLOCK_SIZE);
            func(FlashSim::OpCode::ERASE, chosen_log_addr);
            block_erases.erase(chosen_log->block_id);
            block_erases.insert(std::make_pair(chosen_log->block_id,
                                               min_erase+1));
            data2log_map.insert(std::make_pair(data_block_id, chosen_log));
            return chosen_log->write(wirte_addr, original_lba);
        }

        size_t  clean_data_block_id = 0;
        size_t log_block_id = 0;
        uint16_t min_erase = BLOCK_ERASES;
        LogBlock *log_block = NULL;
        typename std::map<size_t, LogBlock *>::iterator it;
        for (it = data2log_map.begin(); it != data2log_map.end(); it++) {
            if (getEraseNum(it->first) < BLOCK_ERASES && block_erases.find
                    (it->second->block_id)->second < min_erase) {
                log_block = it->second;
                log_block_id = log_block->block_id;
                clean_data_block_id = it->first;
                min_erase = block_erases.find(log_block_id)->second;
            }
        }

        if (log_block == NULL) {
           std::cout << "fail: Cannot find available log block to be "
                   "cleaned!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
//        std::cout << "log chosen to be cleaned: " << log_block_id << std::endl;


        bool need_cleaning_block = false;

        size_t data_block_start = clean_data_block_id * BLOCK_SIZE;
        for (int16_t i = 0; i < BLOCK_SIZE; i++) {
            // Check if the log block has all the valid pages in the data block.
            if (written.find(data_block_start + i) != written.end()
                && log_block->log_page_map.find(i) == log_block->log_page_map.end()) {
                need_cleaning_block = true;
            }
        }

        if (need_cleaning_block) {
            if (clean_erases >= EXCHANGE) {
//                std::cout << "exchangeCleaningBLock "
//                        "result: " << exchangeCleaningBlock(func) << std::endl;
            }
            // Check if the cleaning block has reached its erase limit.
            if (clean_erases >= BLOCK_ERASES) {
               std::cout << "fail: cleaning page can't erase anymore!\n";
                return std::make_pair(FlashSim::ExecState::FAILURE,
                                      FlashSim::Address(0, 0, 0, 0, 0));
            }
            logsFullCleaning(clean_data_block_id, log_block, func, true);
        } else {
            logsFullCleaning(clean_data_block_id, log_block, func, false);
        }
        data2log_map.erase(clean_data_block_id);
        data2log_map.insert(std::make_pair(data_block_id, log_block));
//        std::cout << "before writting in log\n";
        return log_block->write(wirte_addr, original_lba);
    }

    // Perform cleaning when the log block has no empty pages.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyPages(
            const FlashSim::ExecCallBack<PageType> &func, size_t
    data_block_id, FlashSim::Address wirte_addr) {
//        std::cout << "perform noEmptyPages cleaning!\n";
        if (getEraseNum(data_block_id) >= BLOCK_ERASES) {
           std::cout << "fail: No empty data block. Return fail.\n";
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        FlashSim::Address cleaning_addr;


        size_t data_block_start = data_block_id * BLOCK_SIZE;
        typename std::map<size_t, LogBlock *>::iterator it = data2log_map.find(data_block_id);
        LogBlock *log_block = it->second;
        size_t log_block_id = log_block->block_id;
        bool need_cleaning_block = false;

        for (int16_t i = 0; i < BLOCK_SIZE; i++) {
            // Check if the log block has all the valid pages in the data block.
            if (written.find(data_block_start + i) != written.end()
                && garbage.find(data_block_start + i) == garbage.end()
                && log_block->log_page_map.find(i) == log_block->log_page_map.end()) {
                need_cleaning_block = true;
            }
        }

        if (clean_ptr == RAW_CAPACITY / BLOCK_SIZE) {
            clean_ptr --;
            cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
        } else {
            cleaning_addr = lba2addr(clean_ptr * BLOCK_SIZE);
            if (need_cleaning_block) {
                if (clean_erases >= EXCHANGE) {
//                    std::cout << "exchangeCleaningBLock "
//                            "result: " << exchangeCleaningBlock(func) <<
//                                                                      std::endl;
                }
                if (clean_erases >= BLOCK_ERASES) {
                   std::cout << "fail: cleaning page can't erase anymore!\n";
                    return std::make_pair(FlashSim::ExecState::FAILURE,
                                          FlashSim::Address(0, 0, 0, 0, 0));
                }
                func(FlashSim::OpCode::ERASE, cleaning_addr);
                clean_erases++;
            }
        }
//        if (logic2data_map.find(lba / BLOCK_SIZE) == logic2data_map.end
//                ()) {
//            data_block_id = lba / BLOCK_SIZE;
//        } else {
//            data_block_id = logic2data_map.find(lba / BLOCK_SIZE)->second;
//        }

        FlashSim::Address data_addr = lba2addr(data_block_start);

        if (need_cleaning_block) {
//            std::cout << "need cleaning block!\n";

            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
                // Only save the page that has been written before
                if (written.find(data_block_start + i) != written.end()) {
                    if (garbage.find(data_block_start + i) != garbage.end()) {
                        garbage.erase(data_block_start + i);
                        if (wirte_addr.page != i)
                            written.erase(data_block_start + i);
                        continue;
                    }
                    if (wirte_addr.page == i) {
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
                if (written.find(data_block_start + i) != written.end() &&
                    wirte_addr.page != i) {
                    func(FlashSim::OpCode::READ,
                         FlashSim::Address(cleaning_addr.package,
                                           cleaning_addr.die, cleaning_addr.plane,
                                           cleaning_addr.block, i));
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(data_addr.package,
                                           data_addr.die, data_addr.plane,
                                           data_addr.block, i));
                }
            }
        } else {
//            std::cout << "log_page_map size: " << log_block->log_page_map
//                    .size() << std::endl;
//            std::cout << "does not need cleaning block! data block erase "
//                    "times: " << getEraseNum(data_block_id) << "\n";
            func(FlashSim::OpCode::ERASE, data_addr);

            for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
//                std::cout << i << std::endl;
                if (written.find(data_block_start + i) != written.end()) {
                    if (garbage.find(data_block_start + i) != garbage.end()) {
                        garbage.erase(data_block_start + i);
                        if (wirte_addr.page != i)
                            written.erase(data_block_start + i);
                        continue;
                    }
                    if (wirte_addr.page == i) {
                        continue;
                    }
                    std::map<uint16_t, uint16_t>::iterator it_page =
                            log_block->log_page_map.find(i);
                    func(FlashSim::OpCode::READ,
                         FlashSim::Address(log_block->package,
                                           log_block->die, log_block->plane,
                                           log_block->block, it_page->second));
                    func(FlashSim::OpCode::WRITE,
                         FlashSim::Address(data_addr.package,
                                           data_addr.die, data_addr.plane,
                                           data_addr.block, i));
                }
            }
        }
        increaseErase(data_block_id);
        resetLogBlock(log_block);
        if (getEraseNum(log_block->block_id) < BLOCK_ERASES) {
            freed_log.insert(log_block);
        }
        data2log_map.erase(it);
//        std::cout << "before return\n";
        return std::make_pair(FlashSim::ExecState::SUCCESS, wirte_addr);
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
