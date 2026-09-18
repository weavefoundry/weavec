# Counter bounds before reset (RFC 0029)

Reviewed before candidate 55. The exploratory inventory in
`build/rfc29-validation/counter-reset-ranges` and `manifest-original.json`
preserve the original observation: the negative matcher omitted the actual
leak and freed-use wording. This separate inventory corrects those matchers
before implementation; source bytes and accept/reject expectations agree.
A numeric scan count stays unchanged while its formerly equal index is reused.
