// TEST ONLY proof codec/verifier checks. No process state or consensus activation.
#include <test/flowmesh_p2fv_proof.h>
#include <test/flowmesh_fastpath_probe_fixture.h>
#include <hash.h>
#include <util/translation.h>
#include <iostream>
const TranslateFn G_TRANSLATION_FUN{nullptr};
int main()
{
    try {
        ECC_Context ecc;
        fastprobe::Fixture fixture;
        std::vector<p2fv::Member> members;
        for (const auto& key : fixture.seat_keys) {
            members.push_back({key.GetPublicKey().Compressed(), key.SignPoP().Compressed()});
        }
        HashWriter h;
        h << std::string{"B3/TEST-ONLY/P2FV-NATIVE-PROOF-CHECK/1"};
        p2fv::Context context{h.GetHash(), members};
        auto count = p2fv::SelfCheck(context, fixture.seat_keys);
        std::cout << "proof_checks_passed=" << count << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
