# Secure Boot Kernel Module Signing Guide

To insert the `simplefs.ko` kernel module, you can either
disable secure boot or sign the module.

In this guide, we assume you are using Ubuntu, but for
other distributions it just need the same procedures.

## Step 1: Install Dependencies

You need to install linux kernel header, `mokutil`, `kmod`, and `openssl`.

For Ubuntu you can install by these commands:

```bash
$ sudo apt install linux-headers-$(uname -r)
$ sudo apt install mokutil kmod openssl
```

## Step 2: Generate a MOK Key Pair

Create a private and a public certificate (DER format) to sign your modules:

```bash
$ cat << 'EOF' > mokconfig.cnf
[ req ]
default_bits       = 2048
distinguished_name = req_distinguished_name
prompt             = no
x509_extensions    = myexts

[ req_distinguished_name ]
CN = simplefs MOK Signing Key 

[ myexts ]
basicConstraints=CA:FALSE
keyUsage=digitalSignature
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
EOF

openssl req -new -x509 -newkey rsa:2048 -nodes -days 3650 -outform DER -keyout MOK.priv -out MOK.der -config mokconfig.cnf
```

## Step 3. Enroll the Key with MOKutil

Register your new public key with the shim bootloader. You will be prompted to create a temporary password for the next step.

```bash
$ sudo mokutil --import MOK.der
```

## Step 4. Complete Enrollment in UEFI

1. **Reboot** your computer.
2. Upon booting, the blue **MOK Manager** screen will automatically appear.
3. Select **Enroll MOK** -> **View key** to verify your key details, then confirm the enrollment.
4. Enter the temporary **password** you created in Step 3.
5. Continue to boot into Linux.

## Step 5. Sign `simplefs.ko`

```bash
# Sign 'simplefs.ko' 
#$sudo /usr/src/linux-headers-`uname -r`/scripts/sign-file sha256 MOK.priv MOK.der /path/to/simplefs.ko

# Optional: verify the signature is present
$ modinfo /path/to/simplefs.ko | grep -i "signer"
```
