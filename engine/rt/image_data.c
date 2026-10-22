/* the original data image (vm 0x1000-0x89000 of MGSTUS), embedded at build time */
const unsigned char img_data[] = {
#embed "image.bin"
};
const unsigned img_data_size = sizeof img_data;
