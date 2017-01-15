#include "746FlashSim.h"
#include <set>
#include <limits>

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
unsigned int timestamp;

std::set<size_t> written;

std::map<size_t, uint16_t> log_erases;
std::map<unsigned int, size_t> timestamp_to_id;
std::map<size_t, unsigned int> id_to_timestamp;
std::map<size_t, uint16_t> live_pages;

template<typename PageType>
class MyFTL : public FlashSim::FTLBase<PageType> {


public:

    class LogBlock;

    std::set<LogBlock *> free_mapped_logs;
    std::map<size_t, LogBlock *> block_map;

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
            empty_page = 0;
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> read(
                FlashSim::Address *addr) {
            std::map<uint16_t, uint16_t>::iterator it = page_log.find(addr->page);
            if (it != page_log.end()) {
                std::cout << "Found a more recent copy (page " << it->second << ") in logs. Return that copy\n";
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                                      FlashSim::Address(package, die, plane, block,
                                                        it->second));
            } else {
                std::cout << "No more recent copy in logs. Return the calculated PA.\n";
                return std::make_pair(FlashSim::ExecState::SUCCESS, *addr);
            }
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> write(
                FlashSim::Address *addr, uint16_t block_size) {
            std::map<uint16_t, uint16_t>::iterator it = page_log.find(
                    addr->page);
            if (empty_page == block_size) {
                std::cout << "The mapped block is full!\n";
                return std::make_pair(FlashSim::ExecState::FAILURE,
                                      FlashSim::Address(0, 0, 0, 0, 0));
            }
            std::map<size_t, unsigned int>::iterator it2 = id_to_timestamp.find(block_id);
            // Update the maps that save timestamp of each log block.
            if (GC_POLICY != 0){
                if (it2 == id_to_timestamp.end()) {
                    id_to_timestamp.insert(std::make_pair(block_id, timestamp));
                    timestamp_to_id.insert(std::make_pair(timestamp++, block_id));
                    if (timestamp == std::numeric_limits<unsigned int>::max()) {
                        std::cout << "overflow!\n";
                    }
                } else {
                    timestamp_to_id.erase(it2->second);
                    it2->second = timestamp;
                    timestamp_to_id.insert(std::make_pair(timestamp++, block_id));
                    if (timestamp == std::numeric_limits<unsigned int>::max()) {
                        std::cout << "overflow!\n";
                    }
                }
            }
            if (it != page_log.end()) {
                std::cout << "Update the new page id in map: (" << addr->page
                          << ", " << page_log[addr->page] << ")->(" << addr->page
                          << ", " << empty_page << ").\n";
                page_log[addr->page] = empty_page;
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                                      FlashSim::Address(package, die, plane, block,
                                                        empty_page++));
            } else {
                std::cout << "Add new page pair (" << addr->page << ", "
                          << empty_page << ") to the map.\n";
                page_log.insert(std::make_pair(addr->page, empty_page));
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                                      FlashSim::Address(package, die, plane, block,
                                                        empty_page++));
            }
        }
    };

    // Get the block id given a certain lba.
    size_t getBlockId(size_t lba) {
        return lba / BLOCK_SIZE;
    }

    // Check if the block can erase once more.
    bool canErase(size_t block_id) {
        std::map<size_t, uint16_t>::iterator it = log_erases.find(block_id);
        if (it == log_erases.end()) {
            log_erases.insert(std::make_pair(block_id, 1));
        } else {
            if (it->second > BLOCK_ERASES - 10) {
                std::cout << "Block " << block_id << " has been erased " << it->second << " times!\n";
            }
            if (it->second >= BLOCK_ERASES) {
                std::cout << "The block has reached its erase limit!\n";
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
        std::map<size_t, uint16_t>::iterator it = live_pages.find(data_block_id);
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

//        std::cout << "Translate " << ori_lba << " to (" << unsigned(package) << ", " << unsigned(die) << ", "
//                  << plane << ", " << block << ", " << page << ")\n";
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
        GC_POLICY = conf->GetInteger("SELECTED_GC_POLICY");
        RAW_CAPACITY = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE
                       * BLOCK_SIZE;
        ADDRESSABLE = RAW_CAPACITY - RAW_CAPACITY * OVERPROVISIONING / 100;
        empty_log_block = ADDRESSABLE;
        timestamp = 0;
        FIFO_ptr = empty_log_block / BLOCK_SIZE;
        std::cout << "\nSSD_SIZE: " << unsigned(SSD_SIZE) << "\nPACKAGE_SIZE: "
                  << unsigned(PACKAGE_SIZE) << "\nDIE_SIZE: " << DIE_SIZE
                  << "\nPLANE_SIZE: " << PLANE_SIZE << "\nBLOCK_SIZE: "
                  << BLOCK_SIZE << "\nOVERPROVISIONING: " << unsigned(OVERPROVISIONING)
                  << "\nRAW_CAPACITY: " << RAW_CAPACITY << "\nempty_log_block: "
                  << empty_log_block << "\nBLOCK_ERASES: "
                  << BLOCK_ERASES << "\nGC_POLICY: "
                  << unsigned(GC_POLICY) << std::endl;
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
        if (written.find(lba) == written.end()) {
            // The page has never been written before.
            std::cout << "The page hasn't been written before\n";
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
        FlashSim::Address address = mapping(lba);
        // Invalid lba
        if (lba >= ADDRESSABLE) {
            std::cout << "lba out of bound!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
        typename std::map<size_t, LogBlock *>::iterator it = block_map.find(lba / BLOCK_SIZE);
        if (it == block_map.end()) {
            // If no log-reservation block mapped to this data block, return originally calculated PA
            std::cout << "No block mapped to it. Return the calculated PA\n";
            return std::make_pair(FlashSim::ExecState::SUCCESS, address);
        } else {
            // Check if there is a more recent copy in log-reservation blocks.
            LogBlock *logBlock = it->second;
            std::cout << "Found block " << it->first << " mapped to " << logBlock->block_id << "\n";
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
        std::cout << "\nWrite to: " << lba << "\n";
        std::cout << "block map size: " << block_map.size() << "\nfree mapped logs size: " << free_mapped_logs.size()
                  << std::endl;
        // Invalid lba
        if (lba >= ADDRESSABLE) {
            std::cout << "lba out of bound!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
        FlashSim::Address address = mapping(lba);
        size_t data_block_id = lba / BLOCK_SIZE;
        if (written.find(lba) != written.end()) {
            // The page is not empty
            typename std::map<size_t, LogBlock *>::iterator it = block_map.find(data_block_id);
            if (it == block_map.end()) { // No log block mapped to it.
                if (empty_log_block >= (RAW_CAPACITY - BLOCK_SIZE)) {
                    // Log-reservation blocks are all mapped.
                    if (free_mapped_logs.size() > 0) {
                        typename std::set<LogBlock *>::iterator it2 = free_mapped_logs.begin();
                        LogBlock *logBlock = *it2;
                        block_map.insert(std::make_pair(data_block_id, logBlock));
                        std::cout << "Update the block_map: "
                                  << data_block_id << "->" << logBlock->block_id << std::endl;
                        free_mapped_logs.erase(it2);
                        return logBlock->write(&address, BLOCK_SIZE);
                    }
                    std::cout << "Log-reservation blocks are full! Need cleaning\n";
                    return noEmptyLogBlocks(func, lba);
                } else {
                    // Map a new log-reservation block to the data block.
                    std::cout << "Map a new log-reservation block to the data block: "
                              << data_block_id << "->" << getBlockId(empty_log_block) << std::endl;
                    FlashSim::Address new_log_block = mapping(empty_log_block);
                    LogBlock *logBlock = new LogBlock(&new_log_block, empty_log_block / BLOCK_SIZE);
                    empty_log_block += BLOCK_SIZE;
                    block_map.insert(std::make_pair(data_block_id, logBlock));
                    return logBlock->write(&address, BLOCK_SIZE);
                }
            } else {
                // There's a log-reservation block mapped to this data block.
                LogBlock *logBlock = it->second;
                std::cout << "Found " << it->first << " mapped to " << logBlock->block_id << "\n";
                std::pair<FlashSim::ExecState, FlashSim::Address> result = logBlock->write(&address, BLOCK_SIZE);
                if (result.first == FlashSim::ExecState::FAILURE) {
                    return noEmptyPages(func, lba);
                }
                return result;
            }
        } else {
            // The page is empty
            written.insert(lba);
            increaseLivePages(data_block_id);
            return std::make_pair(FlashSim::ExecState::SUCCESS, address);
        }
    }

    // Perform cleaning when the log block has no empty pages.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyPages(
            const FlashSim::ExecCallBack<PageType> &func, size_t lba) {
        FlashSim::Address calculated_addr = mapping(lba);
        // Check if the cleaning block has reached its erase limit.
        if (++clean_erases >= BLOCK_ERASES) {
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        size_t data_block_id = lba / BLOCK_SIZE;
        std::cout << "Cleaning type 1!\n";
        typename std::map<size_t, LogBlock *>::iterator it = block_map.find(data_block_id);
        if (it == block_map.end()) {
            std::cout << "log not found!\n";
        }
        LogBlock *log_block = it->second;
        if (!canErase(data_block_id) || !canErase(log_block->block_id)) {
            // If the data block or the log block can't erase any more.
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
        std::cout << "Clean " << data_block_id << " and " << log_block->block_id << std::endl;
        size_t data_block_page_start = data_block_id * BLOCK_SIZE;
        FlashSim::Address data_addr = mapping(data_block_page_start);
        FlashSim::Address cleaning_addr = mapping(
                RAW_CAPACITY - BLOCK_SIZE);

        std::set<uint16_t> live_pages_set; // Save the live pages' page indices.

        for (uint16_t i = 0; i < BLOCK_SIZE; i++) {
            if (lba == data_block_page_start + i) continue;
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
            }
        }

        func(FlashSim::OpCode::ERASE, data_addr);
        func(FlashSim::OpCode::ERASE, mapping(log_block->block_id * BLOCK_SIZE));

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
            }
        }
        func(FlashSim::OpCode::ERASE, cleaning_addr);
        if (GC_POLICY != 0) {
            std::map<size_t, unsigned int>::iterator it3 = id_to_timestamp.find(log_block->block_id);
            // Update the timestamp related maps.
            if (it3 != id_to_timestamp.end()) {
                timestamp_to_id.erase(it3->second);
                id_to_timestamp.erase(it3);
            }
        }
        resetLogBlock(log_block);
        free_mapped_logs.insert(it->second);
        block_map.erase(it);
        return std::make_pair(FlashSim::ExecState::SUCCESS, calculated_addr);
    }

    // Perform cleaning when there are no empty log blocks.
    std::pair<FlashSim::ExecState, FlashSim::Address> noEmptyLogBlocks(
            const FlashSim::ExecCallBack<PageType> &func, size_t lba) {
        FlashSim::Address calculated_addr = mapping(lba);
        // Check if the cleaning block has reached its erase limit.?
        if (++clean_erases >= BLOCK_ERASES) {
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }

        size_t log_block_id = 0;
        size_t data_block_id = lba / BLOCK_SIZE;
        LogBlock *log_block = NULL;
        std::cout << "Cleaning type 2!\n";
        if (GC_POLICY == 0) { // The FIFO policy.
            std::cout << "000 FIFO policy!\n";
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
            std::cout << "111 LRU policy!\n";
            std::map<unsigned int, size_t>::iterator ts_it1 = timestamp_to_id.begin();
            log_block_id = ts_it1->second;
            id_to_timestamp.erase(ts_it1->second);
            timestamp_to_id.erase(ts_it1);
        } else if (GC_POLICY == 2) {
            std::cout << "222 GREEDY policy!\n";
            std::map<unsigned int, size_t>::iterator ts_it1 = timestamp_to_id.begin();
            std::map<unsigned int, size_t>::iterator ts_it1_chosen;
            std::map<size_t, uint16_t>::iterator pages_it2;
            uint16_t min_pages = UINT16_MAX;
            while (ts_it1 != timestamp_to_id.end()) {
                pages_it2 = live_pages.find(ts_it1->second);
                if (pages_it2->second < min_pages) {
                    log_block_id = pages_it2->first;
                    min_pages = pages_it2->second;
                    ts_it1_chosen = ts_it1;
                }
                ts_it1++;
            }
            id_to_timestamp.erase(ts_it1_chosen->second);
            timestamp_to_id.erase(ts_it1_chosen);
        } else {
            std::cout << "333 COST_BENEFIT policy!\n";
            std::map<unsigned int, size_t>::iterator ts_it1 = timestamp_to_id.begin();
            std::map<unsigned int, size_t>::iterator ts_it1_chosen;
            std::map<size_t, uint16_t>::iterator pages_it2;
            double max_ratio = 0;
            while (ts_it1 != timestamp_to_id.end()) {
                pages_it2 = live_pages.find(ts_it1->second);
                double ratio = double(pages_it2->second) / (2 * BLOCK_SIZE);
                ratio = (1 - ratio) / (1 + ratio) * (timestamp - ts_it1->first + 1);
                if (ratio > max_ratio) {
                    ts_it1_chosen = ts_it1;
                    log_block_id = ts_it1_chosen->second;
                }
                ts_it1++;
            }
            id_to_timestamp.erase(ts_it1_chosen->second);
            timestamp_to_id.erase(ts_it1_chosen);
        }

        std::cout << "log chosen to be cleaned: " << log_block_id << std::endl;
        typename std::map<size_t, LogBlock *>::iterator it;
        for (it = block_map.begin(); it != block_map.end(); it++) {
            std::cout << it->first << "->" << it->second->block_id << std::endl;
            if (it->second->block_id == log_block_id) {
                data_block_id = it->first;
                log_block = it->second;
                break;
            }
        }
        if (it == block_map.end()) std::cout << "Not found! WTF???!!!\n";
        if (!canErase(data_block_id)) {
            // The log block can't be erased any more.
            return std::make_pair(FlashSim::ExecState::FAILURE,
                                  FlashSim::Address(0, 0, 0, 0, 0));
        }
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
            }
        }

        func(FlashSim::OpCode::ERASE, data_addr);
        func(FlashSim::OpCode::ERASE, mapping(log_block_id * BLOCK_SIZE));

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
            }
        }
        func(FlashSim::OpCode::ERASE, cleaning_addr);
        resetLogBlock(log_block);
        block_map.erase(it);
        block_map.insert(std::make_pair(lba / BLOCK_SIZE, log_block));
        return log_block->write(&calculated_addr, lba);
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
FlashSim::FTLBase<TEST_PAGE_TYPE> *FlashSim::CreateMyFTL(const Configuration *conf) {
    return new MyFTL<TEST_PAGE_TYPE>(conf);
}
