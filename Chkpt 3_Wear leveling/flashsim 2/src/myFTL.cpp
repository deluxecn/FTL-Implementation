#include "746FlashSim.h"

template <typename PageType>
class MyFTL : public FlashSim::FTLBase<PageType> {
public:

    /*
     * Constructor
     */
    MyFTL(const FlashSim::Configuration* conf) {
        (void) conf;
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
        (void) lba;
        (void) func;
        return std::make_pair(FlashSim::ExecState::SUCCESS, FlashSim::Address(0, 0, 0, 0, 0));
    }

    /*
     * WriteTranslate() - Translates write address
     *
     * Please refer to ReadTranslate()
     */
    std::pair<FlashSim::ExecState, FlashSim::Address>
    WriteTranslate(size_t lba, const FlashSim::ExecCallBack<PageType> &func) {
        (void) lba;
        (void) func;
        return std::make_pair(FlashSim::ExecState::SUCCESS, FlashSim::Address(0, 0, 0, 0, 0));
    }

    /*
     * Optionally mark a LBA as a garbage.
     */
    FlashSim::ExecState
    Trim(size_t lba, const FlashSim::ExecCallBack<PageType>& func) {
        (void) lba;
        (void) func;
        return FlashSim::ExecState::SUCCESS;
    }
};

/*
 * CreateMyFTL() - Creates class MyFTL object
 *
 * You do not need to modify this
 */
FlashSim::FTLBase<TEST_PAGE_TYPE>* FlashSim::CreateMyFTL(const Configuration* conf) {
    return new MyFTL<TEST_PAGE_TYPE>(conf);
}
