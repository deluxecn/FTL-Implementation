#include "746FlashSim.h"
#include <set>

uint8_t SSD_SIZE;
uint8_t PACKAGE_SIZE;
uint16_t DIE_SIZE;
uint16_t PLANE_SIZE;
uint16_t BLOCK_SIZE;
uint16_t BLOCK_ERASES;
uint8_t OVERPROVISIONING;
uint8_t GC_POLICY;

size_t RAW_CAPACITY;
size_t ADDRESSABLE;
size_t empty_log_block;
size_t FIFO_ptr;
uint16_t clean_erases = 0;
unsigned int timestamp = 0;

std::set<size_t> written;

std::map<size_t, uint16_t> log_erases; // <log_block_id, erases_num>
std::map<unsigned int, size_t> timestamp_to_id; // <timestamp, log_block_id>
std::map<size_t, unsigned int> id_to_timestamp; // <log_block_id, timestamp>
std::map<size_t, uint16_t> live_pages; // <data_block_id, live_pages_num>

template<typename PageType>
class MyFTL : public FlashSim::FTLBase<PageType> {

public:

    class LogBlock;

    std::map<size_t, LogBlock *> block_map; // <data_block_id, LogBlock *>

    class LogBlock {
    public:
        uint8_t package;
        uint8_t die;
        uint16_t plane;
        uint16_t block;
        size_t block_id;
        uint16_t empty_page;
        std::map<uint16_t, uint16_t> page_log;

        LogBlock(FlashSim::Address *addr, size_t block_id) {
            package = addr->package;
            die = addr->die;
            plane = addr->plane;
            block = addr->block;
            this->block_id = block_id;
            empty_page = 1;
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> read(
                FlashSim::Address *addr) {
            std::map<uint16_t, uint16_t>::iterator it = page_log.find(
                    addr->page);
            if (it != page_log.end()) {
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                                      FlashSim::Address(package, die, plane,
                                                        block,
                                                        it->second));
            } else {
                return std::make_pair(FlashSim::ExecState::SUCCESS, *addr);
            }
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> write(
                FlashSim::Address *addr) {
            if (empty_page == BLOCK_SIZE) {
                return std::make_pair(FlashSim::ExecState::FAILURE,
                                      FlashSim::Address(0, 0, 0, 0, 0));
            }
            // Update the maps that save timestamp of each log block.
            if (GC_POLICY == 1 || GC_POLICY == 3) {
                std::map<size_t, unsigned int>::iterator it_id2ts
                        = id_to_timestamp.find(block_id);
                if (it_id2ts == id_to_timestamp.end()) {
                    id_to_timestamp.insert(std::make_pair(block_id,
                                                          timestamp));
                    timestamp_to_id.insert(std::make_pair(timestamp++,
                                                          block_id));
                } else {
                    timestamp_to_id.erase(it_id2ts->second);
                    it_id2ts->second = timestamp;
                    timestamp_to_id.insert(std::make_pair(timestamp++,
                                                          block_id));
                }
            }
            // Update the page log.
            std::map<uint16_t, uint16_t>::iterator it = page_log.find(
                    addr->page);
            if (it != page_log.end()) {
                it->second = empty_page;
            } else {
                page_log.insert(std::make_pair(addr->page, empty_page));
            }
            return std::make_pair(FlashSim::ExecState::SUCCESS,
                                  FlashSim::Address(package, die, plane, block,
                                                    empty_page++));
        }
    };

    // Check if the block can erase once more.
    bool canErase(size_t block_id) {
        std::map<size_t, uint16_t>::iterator it = log_erases.find(block_id);
        if (it == log_erases.end()) {
            log_erases.insert(std::make_pair(block_id, 1));
        } else {
            if (it->second >= BLOCK_ERASES) {
                return false;
            } else {
                it->second++;
            }
        }
        return true;
    }

    // Reset the elements of a certain log block.
    void resetLogBlock(LogBlock *log_block) {
        log_block->empty_page = 0;
        log_block->page_log.clear();
    }

    // Increase the live pages of a certain data block by one.
    void increaseLivePages(size_t data_block_id) {
        std::map<size_t, uint16_t>::iterator it
                = live_pages.find(data_block_id);
        if (it == live_pages.end()) {
            live_pages.insert(std::make_pair(data_block_id, 1));
        } else {
            it->second++;
        }
    }

    /*
     * Map LBA into PBA.
     * Return FlashSim::Address
     */
    FlashSim::Address mapping(size_t lba) {
//        size_t ori_lba = lba;
        size_t tmp1 = (size_t) PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE
                      * BLOCK_SIZE;
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
        GC_POLICY = 3;
        RAW_CAPACITY = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE
                       * BLOCK_SIZE;
        ADDRESSABLE = RAW_CAPACITY - RAW_CAPACITY * OVERPROVISIONING / 100;
        empty_log_block = ADDRESSABLE;
        FIFO_ptr = empty_log_block / BLOCK_SIZE;
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
        if (written.find(lba) == written.end()) {
            // The page has never been written before.
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        // Invalid lba
        if (lba >= ADDRESSABLE) {
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        FlashSim::Address address = mapping(lba);
        typename std::map<size_t, LogBlock *>::iterator it = block_map.find(
                lba / BLOCK_SIZE);
        if (it == block_map.end()) {
            // If no log-reservation block mapped to this data block,
            // return originally calculated PA
            return std::make_pair(FlashSim::ExecState::SUCCESS, address);
        } else {
            // Check if there is a more recent copy in log-reservation blocks.
            LogBlock *logBlock = it->second;
            return logBlock->read(&address);
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
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        FlashSim::Address address = mapping(lba);
        size_t data_block_id = lba / BLOCK_SIZE;
        if (written.find(lba) != written.end()) {
            // The page is not empty
            typename std::map<size_t, LogBlock *>::iterator it = block_map.find(
                    data_block_id);
            if (it == block_map.end()) { // No log block mapped to it.
                if (empty_log_block >= (RAW_CAPACITY - BLOCK_SIZE)) {
                    // Log-reservation blocks are all mapped.
                    return noEmptyLogBlocks(func, lba);
                } else {
                    // Map a new log-reservation block to the data block.
                    FlashSim::Address new_log_block = mapping(empty_log_block);
                    LogBlock *logBlock = new LogBlock(&new_log_block,
                                                      empty_log_block /
                                                      BLOCK_SIZE);
                    empty_log_block += BLOCK_SIZE;
                    block_map.insert(std::make_pair(data_block_id, logBlock));
                    return logBlock->write(&address);
                }
            } else {
                // There's a log-reservation block mapped to this data block.
                LogBlock *logBlock = it->second;
                std::pair<FlashSim::ExecState, FlashSim::Address> result
                        = logBlock->write(&address);
                if (result.first == FlashSim::ExecState::FAILURE) {
                    return noEmptyPages(func, lba);
                }
                return result;
            }
        } else {
            // The page is empty
            written.insert(lba);
            increaseLivePages(data_block_id);
            timestamp++;
            return std::make_pair(FlashSim::ExecState::SUCCESS, address);
        }
    }

    // Perform standard cleaning given the data block and log block.
    void standardCleaning(size_t data_block_id, LogBlock *log_block,
                          const FlashSim::ExecCallBack<PageType> &func) {
        size_t data_block_page_start = data_block_id * BLOCK_SIZE;
        FlashSim::Address data_addr = mapping(data_block_page_start);
        FlashSim::Address cleaning_addr = mapping(
                RAW_CAPACITY - BLOCK_SIZE);
        std::set<uint16_t> live_pages_set; // Save the live pages' page indices.

        for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
            // Only save the page that has been written before
            if (written.find(data_block_page_start + i) != written.end()) {
                live_pages_set.insert(i);
                std::map<uint16_t, uint16_t>::iterator it2 =
                        log_block->page_log.find(i);
                if (it2 == log_block->page_log.end()) {
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
                timestamp++;
            }
        }

        func(FlashSim::OpCode::ERASE, data_addr);
        func(FlashSim::OpCode::ERASE,
             mapping(log_block->block_id * BLOCK_SIZE));

        // Copy live pages back to the data block
        for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
            if (live_pages_set.find(i) != live_pages_set.end()) {
                func(FlashSim::OpCode::READ,
                     FlashSim::Address(cleaning_addr.package,
                                       cleaning_addr.die, cleaning_addr.plane,
                                       cleaning_addr.block, i));
                func(FlashSim::OpCode::WRITE,
                     FlashSim::Address(data_addr.package,
                                       data_addr.die, data_addr.plane,
                                       data_addr.block, i));
                timestamp++;
            }
        }
        func(FlashSim::OpCode::ERASE, cleaning_addr);
    }

    // Perform optimized cleaning given the data block and log block.
    void optimizedCleaning(size_t data_block_id, LogBlock *log_block,
                           const FlashSim::ExecCallBack<PageType> &func) {
        size_t data_block_page_start = data_block_id * BLOCK_SIZE;
        FlashSim::Address data_addr = mapping(data_block_page_start);

        func(FlashSim::OpCode::ERASE, data_addr);

        std::map<uint16_t, uint16_t>::iterator it_page
                = log_block->page_log.begin();

        while (it_page != log_block->page_log.end()) {
            func(FlashSim::OpCode::READ,
                 FlashSim::Address(log_block->package,
                                   log_block->die, log_block->plane,
                                   log_block->block, it_page->second));
            func(FlashSim::OpCode::WRITE,
                 FlashSim::Address(data_addr.package,
                                   data_addr.die, data_addr.plane,
                                   data_addr.block, it_page->first));
            timestamp++;
            it_page++;
        }
        func(FlashSim::OpCode::ERASE,
             mapping(log_block->block_id * BLOCK_SIZE));
    }

    // Perform cleaning when the log block has no empty pages.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyPages(
            const FlashSim::ExecCallBack<PageType> &func, size_t lba) {
        FlashSim::Address calculated_addr = mapping(lba);
        size_t data_block_id = lba / BLOCK_SIZE;
        size_t data_block_start = data_block_id * BLOCK_SIZE;
        typename std::map<size_t, LogBlock *>::iterator it = block_map.find(
                data_block_id);
        LogBlock *log_block = it->second;
        if (!canErase(data_block_id) || !canErase(log_block->block_id)) {
            // If the data block or the log block can't erase any more.
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        bool flag = true;
        for (int16_t i = 0; i < BLOCK_SIZE; i++) {
            // Check if the log block has all the valid pages in the data block.
            if (written.find(data_block_start + i) != written.end()
                && log_block->page_log.find(i) == log_block->page_log.end()) {
                flag = false;
            }
        }
        if (flag) {
            optimizedCleaning(data_block_id, log_block, func);
        } else {
            // Check if the cleaning block has reached its erase limit.
            if (++clean_erases > BLOCK_ERASES) {
                return std::make_pair(FlashSim::ExecState::FAILURE,
                                      FlashSim::Address(0, 0, 0, 0, 0));
            }
            standardCleaning(data_block_id, log_block, func);
        }
        resetLogBlock(log_block);
        return log_block->write(&calculated_addr);
    }

    // Perform cleaning when there are no empty log blocks.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyLogBlocks(
            const FlashSim::ExecCallBack<PageType> &func, size_t lba) {
        FlashSim::Address calculated_addr = mapping(lba);
        size_t clean_data_block_id = 0;
        size_t log_block_id = 0;
        LogBlock *log_block = NULL;
        if (GC_POLICY == 0) { // The FIFO policy.
            if (!canErase(FIFO_ptr)) {
                // The log block can't be erased any more.
                return std::make_pair(FlashSim::ExecState::FAILURE,
                                      FlashSim::Address(0, 0, 0, 0, 0));
            }
            log_block_id = FIFO_ptr;
            FIFO_ptr++;
            if (FIFO_ptr == (RAW_CAPACITY / BLOCK_SIZE - 1)) {
                FIFO_ptr = ADDRESSABLE / BLOCK_SIZE;
            }
        } else if (GC_POLICY == 1) {
            std::map<unsigned int, size_t>::iterator it_ts2id
                    = timestamp_to_id.begin();
            log_block_id = it_ts2id->second;
            id_to_timestamp.erase(log_block_id);
            timestamp_to_id.erase(it_ts2id);
        } else if (GC_POLICY == 2) {
            std::map<size_t, uint16_t>::iterator it_pages = live_pages.begin();
            uint16_t min_pages = UINT16_MAX;
            while (it_pages != live_pages.end()) {
                if (it_pages->second < min_pages) {
                    clean_data_block_id = it_pages->first;
                    min_pages = it_pages->second;
                }
                it_pages++;
            }
        } else {
            std::map<unsigned int, size_t>::iterator ts_it1
                    = timestamp_to_id.begin();
            std::map<unsigned int, size_t>::iterator ts_it1_chosen;
            typename std::map<size_t, LogBlock *>::iterator it_block_map;
            std::map<size_t, uint16_t>::iterator pages_it2 = live_pages.begin();
            double max_ratio = 0;
            while (ts_it1 != timestamp_to_id.end()) {
                for (it_block_map = block_map.begin();
                     it_block_map != block_map.end(); it_block_map++) {
                    if (it_block_map->second->block_id == ts_it1->second) {
                        pages_it2 = live_pages.find(it_block_map->first);
                    }
                }
                double ratio = double(pages_it2->second) / (2 * BLOCK_SIZE);
                ratio = (1 - ratio) / (1 + ratio) * (timestamp - ts_it1->first);
                if (ratio > max_ratio) {
                    ts_it1_chosen = ts_it1;
                    clean_data_block_id = pages_it2->first;
                    max_ratio = ratio;
                }
                ts_it1++;
            }
            id_to_timestamp.erase(ts_it1_chosen->second);
            timestamp_to_id.erase(ts_it1_chosen);
        }

        typename std::map<size_t, LogBlock *>::iterator it;
        if (GC_POLICY == 2 || GC_POLICY == 3) {
            it = block_map.find(clean_data_block_id);
            log_block = it->second;
            log_block_id = log_block->block_id;
        } else {
            for (it = block_map.begin(); it != block_map.end(); it++) {
                if (it->second->block_id == log_block_id) {
                    clean_data_block_id = it->first;
                    log_block = it->second;
                    break;
                }
            }
        }
        if (!canErase(clean_data_block_id) || !canErase(log_block_id)) {
            // The log block can't be erased any more.
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        bool flag = true;
        size_t data_block_start = clean_data_block_id * BLOCK_SIZE;
        for (int16_t i = 0; i < BLOCK_SIZE; i++) {
            // Check if the log block has all the valid pages in the data block.
            if (written.find(data_block_start + i) != written.end()
                && log_block->page_log.find(i) == log_block->page_log.end()) {
                flag = false;
            }
        }
        if (flag) {
            optimizedCleaning(clean_data_block_id, log_block, func);
        } else {
            // Check if the cleaning block has reached its erase limit.
            if (++clean_erases > BLOCK_ERASES) {
                return std::make_pair(FlashSim::ExecState::FAILURE,
                                      FlashSim::Address(0, 0, 0, 0, 0));
            }
            standardCleaning(clean_data_block_id, log_block, func);
        }
        resetLogBlock(log_block);
        block_map.erase(it);
        block_map.insert(std::make_pair(lba / BLOCK_SIZE, log_block));
        return log_block->write(&calculated_addr);
    }

    /*
     * Optionally mark a LBA as a garbage.
     */
    FlashSim::ExecState
    Trim(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        return FlashSim::ExecState::SUCCESS;
    }
};

/*
 * CreateMyFTL() - Creates class MyFTL object
 *
 * You do not need to modify this
 */
FlashSim::FTLBase<TEST_PAGE_TYPE> *
FlashSim::CreateMyFTL(const Configuration *conf) {
    return new MyFTL<TEST_PAGE_TYPE>(conf);
}
