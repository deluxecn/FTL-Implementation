#include "746FlashSim.h"
#include <set>

using namespace std;

uint8_t SSD_SIZE;
uint8_t PACKAGE_SIZE;
uint16_t DIE_SIZE;
uint16_t PLANE_SIZE;
uint16_t BLOCK_SIZE;
uint8_t BLOCK_ERASES;
uint8_t OVERPROVISIONING;

size_t ADDRESSABLE;
size_t BLOCK_NUM;
size_t empty_block;

set<size_t> written; // Store the lba that has been written.
set<size_t> freed_block; // Store the blocks that


// Every data block and log block in used is a Block instance.
class Block {
public:
    size_t block_id;
    Block *log_block;
    map<uint16_t, uint16_t> page_map; // Map logical page # to physical page #
    uint16_t empty_page_start;

    Block(size_t block_id) {
        this->block_id = block_id;
        empty_page_start = 0;
        log_block = NULL;
    }
};

map<size_t, uint8_t> block_erases; // Store erase count of each block.

map<size_t, Block *> logic2data_map;

template<typename PageType>
class MyFTL : public FlashSim::FTLBase<PageType> {
public:

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
        size_t RAW_CAPACITY = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE
                              * BLOCK_SIZE;
        ADDRESSABLE = RAW_CAPACITY - RAW_CAPACITY * OVERPROVISIONING / 100;
        BLOCK_NUM = RAW_CAPACITY / BLOCK_SIZE;
        empty_block = 0;
    }

    /*
     * Destructor - Plase keep it as virtual to allow destroying the
     *              object with base type pointer
     */
    virtual ~MyFTL() {
    }

    // Get the FlashSim::Address given a lba.
    FlashSim::Address lba2addr(size_t lba) {
        size_t tmp1 =
                (size_t) PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * BLOCK_SIZE;
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

    // Get the FlashSim::Address given data block and page #.
    FlashSim::Address page2addr(size_t block_id, uint8_t page) {
        return lba2addr(block_id * BLOCK_SIZE + page);
    }

    // Get the erase count given the block id.
    uint16_t getEraseNum(size_t block_id) {
        map<size_t, uint8_t>::iterator it = block_erases.find(block_id);
        if (it != block_erases.end()) {
            return it->second;
        } else {
            block_erases.insert(make_pair(block_id, 0));
            return 0;
        }

    }

    // Increase erase count by one.
    void increaseErase(size_t block_id) {
        map<size_t, uint8_t>::iterator it = block_erases.find(block_id);
        if (it == block_erases.end()) {
            block_erases.insert(make_pair(block_id, 1));
        } else {
            it->second++;
        }
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
    pair<FlashSim::ExecState, FlashSim::Address>
    ReadTranslate(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        if (written.find(lba) == written.end()) {
            // The page has never been written before.
            return make_pair(FlashSim::ExecState::FAILURE,
                             FlashSim::Address(0, 0, 0, 0, 0));
        }
        if (lba >= ADDRESSABLE) {
            // Invalid lba
            return make_pair(FlashSim::ExecState::FAILURE,
                             FlashSim::Address(0, 0, 0, 0, 0));
        }
        uint8_t logic_page = lba % BLOCK_SIZE;
        Block *data_block = logic2data_map.find(lba / BLOCK_SIZE)->second;
        if (data_block->page_map.find(logic_page) != data_block->page_map.end
                ()) {
            uint8_t p_page = data_block->page_map.find(logic_page)->second;
            return make_pair(FlashSim::ExecState::SUCCESS,
                             page2addr(data_block->block_id, p_page));
        } else {
            Block *log_block = data_block->log_block;
            if (log_block == NULL) {
            }
            if (log_block->page_map.find(logic_page) == log_block->page_map
                    .end()) {
            }
            uint8_t p_page = log_block->page_map.find(logic_page)->second;
            return make_pair(FlashSim::ExecState::SUCCESS,
                             page2addr(log_block->block_id, p_page));
        }
    }

    /*
     * WriteTranslate() - Translates write address
     *
     * Please refer to ReadTranslate()
     */
    pair<FlashSim::ExecState, FlashSim::Address>
    WriteTranslate(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        if (lba >= ADDRESSABLE) {
            return make_pair(FlashSim::ExecState::FAILURE,
                             FlashSim::Address(0, 0, 0, 0, 0));
        }
        size_t logic_block_id = lba / BLOCK_SIZE;
        if (logic2data_map.find(logic_block_id) != logic2data_map.end()) {
            return writeHasDataBlock(
                    logic2data_map.find(logic_block_id)->second,
                    lba, func);
        } else {
            return writeNoDataBlock(lba, func);
        }
    }

    // When no data block exists for the request lba.
    pair<FlashSim::ExecState, FlashSim::Address>
    writeNoDataBlock(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        uint8_t logic_page = lba % BLOCK_SIZE;
        Block *data_block = getEmptyBlock(func);
        if (data_block == NULL) {
            // No log block.
            return make_pair(FlashSim::ExecState::FAILURE,
                             FlashSim::Address(0, 0, 0, 0, 0));
        }
        data_block->page_map.insert(make_pair(logic_page, 0));
        logic2data_map.insert(make_pair(lba / BLOCK_SIZE, data_block));
        data_block->empty_page_start++;
        written.insert(lba);
        return make_pair(FlashSim::ExecState::SUCCESS, page2addr
                (data_block->block_id, 0));
    };

    // When the data block exists.
    pair<FlashSim::ExecState, FlashSim::Address>
    writeHasDataBlock(Block *data_block, size_t lba, const
    FlashSim::ExecCallBack<PageType>
    &func) {
        uint8_t logic_page = lba % BLOCK_SIZE;
        if (data_block->empty_page_start == BLOCK_SIZE) {
            // No empty page in data block.
            if (data_block->log_block != NULL) {
                return writeLog(func, data_block, data_block->log_block,
                                lba);
            }
            Block *log = getEmptyBlock(func);
            if (log == NULL) {
                // No log block.
                return make_pair(FlashSim::ExecState::FAILURE,
                                 FlashSim::Address(0, 0, 0, 0, 0));
            } else {
                data_block->log_block = log;
                return writeLog(func, data_block, log, lba);
            }
        } else {
            // There is still an empty page.
            data_block->page_map.erase(logic_page);
            data_block->page_map.insert(make_pair(logic_page,
                                                  data_block->empty_page_start));
            written.insert(lba);
            return make_pair(FlashSim::ExecState::SUCCESS,
                             page2addr(data_block->block_id,
                                       data_block->empty_page_start++));
        }
    };

    // Write data to log block.
    pair<FlashSim::ExecState, FlashSim::Address> writeLog
            (const
             FlashSim::ExecCallBack<PageType>
             &func, Block *data_block, Block *log, size_t lba) {
        if (log->empty_page_start == BLOCK_SIZE) {
            return make_pair(FlashSim::ExecState::FAILURE,
                             FlashSim::Address(0, 0, 0, 0, 0));
        } else {
            uint8_t logic_page = lba % BLOCK_SIZE;
            data_block->page_map.erase(logic_page);
            log->page_map.erase(logic_page);
            log->page_map.insert(make_pair
                                         (logic_page,
                                          log->empty_page_start));
            if (data_block->page_map.size() == 0) {
                // Data block doesn't have live pages.
                if (getEraseNum(data_block->block_id) < BLOCK_ERASES) {
                    freed_block.insert(data_block->block_id);
                }
                logic2data_map.find(lba / BLOCK_SIZE)->second = log;
                delete data_block;
            }
            written.insert(lba);
            return make_pair(FlashSim::ExecState::SUCCESS, page2addr
                    (log->block_id, log->empty_page_start++));
        }
    };

    // Try to find an empty or unused block.
    Block *getEmptyBlock(const
                         FlashSim::ExecCallBack<PageType>
                         &func) {
        Block *block = NULL;
        if (empty_block == BLOCK_NUM) {
            if (freed_block.size() < 1) {
                return NULL;
            }
            typename set<size_t>::iterator it;
            it = freed_block.begin();
            size_t chosen = *it;
            uint16_t min_erase = getEraseNum(chosen);
            it++;
            while (it != freed_block.end()) {
                if (getEraseNum(*it) <
                    min_erase) {
                    min_erase = getEraseNum(*it);
                    chosen = *it;
                }
                it++;
            }
            freed_block.erase(chosen);
            func(FlashSim::OpCode::ERASE, lba2addr(chosen * BLOCK_SIZE));
            increaseErase(chosen);
            block = new Block(chosen);
        } else {
            block = new Block(empty_block);
            block_erases.insert(make_pair(empty_block, 0));
            empty_block++;
        }
        return block;
    }

    pair<FlashSim::ExecState, FlashSim::Address> noEmptyPages
            (const
             FlashSim::ExecCallBack<PageType>
             &func, Block *data_block, Block *log, size_t lba) {
        Block *new_block = getEmptyBlock(func);
        if (new_block == NULL) {
            return make_pair(FlashSim::ExecState::FAILURE,
                             FlashSim::Address(0, 0, 0, 0, 0));
        } else {
            return clean(func, data_block, log, new_block, lba);
        }
    }

    // Segment cleaning.
    // Copy the live pages from data block and log to an empty block.
    pair<FlashSim::ExecState, FlashSim::Address> clean(
            const
            FlashSim::ExecCallBack<PageType>
            &func, Block *data_block, Block *log, Block *new_block, size_t lba

    ) {
        uint16_t logic_page = lba % BLOCK_SIZE;

        map<uint16_t, uint16_t>::iterator page_it;
        for (page_it = data_block->page_map.begin();
             page_it != data_block->page_map.end(); page_it++) {
            if (page_it->first == logic_page) continue;
            func(FlashSim::OpCode::READ, page2addr(data_block->block_id,
                                                   page_it->second));
            func(FlashSim::OpCode::WRITE,
                 page2addr(new_block->block_id, new_block->empty_page_start));
            new_block->page_map.insert(make_pair(page_it->first,
                                                 new_block->empty_page_start++));
        }

        for (page_it = log->page_map.begin();
             page_it != log->page_map.end(); page_it++) {
            if (page_it->first == logic_page) continue;
            func(FlashSim::OpCode::READ, page2addr(log->block_id,
                                                   page_it->second));
            func(FlashSim::OpCode::WRITE,
                 page2addr(new_block->block_id, new_block->empty_page_start));
            new_block->page_map.insert(make_pair(page_it->first,
                                                 new_block->empty_page_start++));
        }

        if (getEraseNum(data_block->block_id) < BLOCK_ERASES) {
            freed_block.insert(data_block->block_id);
        }

        if (getEraseNum(log->block_id) < BLOCK_ERASES) {
            freed_block.insert(log->block_id);
        }

        logic2data_map.find(lba / BLOCK_SIZE)->second = new_block;

        delete data_block;
        delete log;

        new_block->page_map.insert(make_pair(logic_page,
                                             new_block->empty_page_start));
        written.insert(lba);
        return make_pair(FlashSim::ExecState::SUCCESS,
                         page2addr(new_block->block_id,
                                   new_block->empty_page_start++));
    };

    /*
     * Optionally mark a LBA as a garbage.
     */
    FlashSim::ExecState
    Trim(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        if (written.find(lba) != written.end()) {
            written.erase(lba);
            uint16_t page = lba % BLOCK_SIZE;
            if (logic2data_map.find(lba / BLOCK_SIZE) != logic2data_map.end()) {
                Block *data_block = logic2data_map.find(lba / BLOCK_SIZE)
                        ->second;
                Block *log = data_block->log_block;
                data_block->page_map.erase(page);
                if (log == NULL) {
                    if (data_block->page_map.size() == 0
                        && data_block->empty_page_start == BLOCK_SIZE) {
                        // Data block doesn't have live pages.
                        if (getEraseNum(data_block->block_id) < BLOCK_ERASES) {
                            freed_block.insert(data_block->block_id);
                        }
                        logic2data_map.erase(lba / BLOCK_SIZE);
                        delete data_block;
                    }
                } else {
                    log->page_map.erase(page);
                    if (data_block->page_map.size() == 0
                        && data_block->empty_page_start == BLOCK_SIZE) {
                        // Data block doesn't have live pages.
                        if (getEraseNum(data_block->block_id) < BLOCK_ERASES) {
                            freed_block.insert(data_block->block_id);
                        }
                        logic2data_map.find(lba / BLOCK_SIZE)->second = log;
                        delete data_block;
                    } else {
                        if (log->page_map.size() == 0
                            && log->empty_page_start == BLOCK_SIZE) {
                            // Log block doesn't have live pages.
                            if (getEraseNum(log->block_id) < BLOCK_ERASES) {
                                freed_block.insert(log->block_id);
                            }
                            delete log;
                            data_block->log_block = NULL;
                        }
                    }
                }
            }
        }
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
