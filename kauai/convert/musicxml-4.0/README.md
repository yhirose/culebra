# MusicXML 4.0 schema

`musicxml.xsd`, `container.xsd` (for an `.mxl`'s `META-INF/container.xml`),
`xlink.xsd` and `xml.xsd` are the MusicXML 4.0 W3C XML Schema, unchanged
but for LF line ends, from the `schema/` folder of
[w3c/musicxml](https://github.com/w3c/musicxml) at tag `v4.0`. They are
published by the W3C Music Notation Community Group under the
[W3C Community Final Specification Agreement](https://www.w3.org/community/about/agreements/final/);
each file keeps its notice. `catalog.xml` is ours.

`kauai/convert/test_musicxml.cul` checks every score the converter writes
(and the container of an `.mxl`) against them:

```sh
XML_CATALOG_FILES=kauai/convert/musicxml-4.0/catalog.xml \
  xmllint --nonet --noout --schema kauai/convert/musicxml-4.0/musicxml.xsd score.musicxml
```
