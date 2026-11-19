package com.vemod.container.proxy

import android.content.ComponentName
import android.content.Intent
import android.content.ServiceConnection
import android.os.Binder
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Parcel
import android.util.Log

object MockBillingService {
    private const val TAG = "MockBillingService"
    private val mainHandler = Handler(Looper.getMainLooper())

    fun isBillingOrLicensing(intent: Intent?): Boolean {
        if (intent == null) return false
        val action = intent.action ?: ""
        val pkg = intent.component?.packageName ?: intent.`package` ?: ""
        return action.contains("billing", ignoreCase = true) ||
               action.contains("licensing", ignoreCase = true) ||
               pkg.contains("com.android.vending") ||
               pkg.contains("billing", ignoreCase = true) ||
               pkg.contains("licensing", ignoreCase = true)
    }

    fun mockBind(intent: Intent, conn: ServiceConnection): Boolean {
        Log.i(TAG, "mockBind: intercepting billing/licensing bind for $intent")
        val comp = intent.component ?: ComponentName(
            intent.`package` ?: "com.android.vending",
            "com.android.vending.billing.InAppBillingService"
        )
        mainHandler.post {
            try {
                conn.onServiceConnected(comp, MockBillingBinder())
                Log.i(TAG, "mockBind: dispatched onServiceConnected for $comp")
            } catch (t: Throwable) {
                Log.w(TAG, "mockBind: onServiceConnected callback failed", t)
            }
        }
        return true
    }

    class MockBillingBinder : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            try {
                data.setDataPosition(0)
                val descriptor = data.readString() ?: ""
                reply?.writeNoException()

                if (descriptor.contains("ILicensingService", ignoreCase = true)) {
                    val nonce = data.readLong()
                    val pkg = data.readString()
                    val listenerBinder = data.readStrongBinder()
                    if (listenerBinder != null) {
                        mainHandler.post {
                            val pData = Parcel.obtain()
                            val pReply = Parcel.obtain()
                            try {
                                pData.writeInterfaceToken("com.android.vending.licensing.ILicenseResultListener")
                                pData.writeInt(0x0100) // LICENSED (256)
                                pData.writeString("licensed_data")
                                pData.writeString("signature")
                                listenerBinder.transact(1, pData, pReply, 0)
                            } catch (t: Throwable) {
                                Log.w(TAG, "Licensing callback transact error: ${t.message}")
                            } finally {
                                pData.recycle()
                                pReply.recycle()
                            }
                        }
                    }
                    return true
                }

                // InAppBillingService transactions:
                // Code 1: isBillingSupported -> int (0 = BILLING_RESPONSE_RESULT_OK)
                // Code 5: consumePurchase -> int (0 = BILLING_RESPONSE_RESULT_OK)
                // Other codes (2, 3, 4): getSkuDetails, getBuyIntent, getPurchases -> Bundle
                if (reply != null) {
                    if (code == 1 || code == 5) {
                        reply.writeInt(0) // BILLING_RESPONSE_RESULT_OK
                    } else {
                        reply.writeInt(1) // non-null Bundle marker
                        val b = Bundle().apply {
                            putInt("RESPONSE_CODE", 0) // RESULT_OK
                            putStringArrayList("DETAILS_LIST", ArrayList<String>())
                            putStringArrayList("INAPP_PURCHASE_ITEM_LIST", ArrayList<String>())
                            putStringArrayList("INAPP_PURCHASE_DATA_LIST", ArrayList<String>())
                            putStringArrayList("INAPP_DATA_SIGNATURE_LIST", ArrayList<String>())
                        }
                        b.writeToParcel(reply, 0)
                    }
                }
                return true
            } catch (t: Throwable) {
                Log.w(TAG, "MockBillingBinder transact error", t)
                reply?.writeNoException()
                reply?.writeInt(0)
                return true
            }
        }
    }
}
